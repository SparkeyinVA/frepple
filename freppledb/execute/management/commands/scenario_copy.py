#
# Copyright (C) 2010-2019 by frePPLe bv
#
# This library is free software; you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero
# General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public
# License along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

import os
import subprocess
from datetime import datetime

from django.core.management import call_command
from django.core.management.base import BaseCommand, CommandError
from django.conf import settings
from django.db import DEFAULT_DB_ALIAS
from django.utils.translation import gettext_lazy as _
from django.template.loader import render_to_string

from freppledb.execute.models import Task, ScheduledTask
from freppledb.execute.views import FileManager
from freppledb.common.middleware import _thread_locals
from freppledb.common.models import User, Scenario, Parameter
from freppledb.input.models import Item
from freppledb import __version__


class Command(BaseCommand):
    help = """
        This command copies the contents of a database into another.
        The original data in the destination database are lost.

        The pg_dump and psql commands need to be in the path, otherwise
        this command will fail.
        """

    requires_system_checks = []

    def get_version(self):
        return __version__

    def add_arguments(self, parser):
        parser.add_argument("--user", help="User running the command"),
        parser.add_argument(
            "--force",
            action="store_true",
            default=False,
            help="Overwrite scenarios already in use",
        )
        parser.add_argument(
            "--description", help="Description of the destination scenario"
        )
        parser.add_argument(
            "--task",
            type=int,
            help="Task identifier (generated automatically if not provided)",
        )
        parser.add_argument(
            "--database",
            default=DEFAULT_DB_ALIAS,
            help="Unused argument for this command",
        )
        parser.add_argument(
            "--promote",
            action="store_true",
            default=False,
            help="promotes a scenario to production",
        )
        parser.add_argument(
            "--dumpfile", default=None, help="specifies source dump file"
        )
        parser.add_argument("source", help="source database to copy")
        parser.add_argument("destination", help="destination database to copy")

    def handle(self, **options):
        # Make sure the debug flag is not set!
        # When it is set, the django database wrapper collects a list of all sql
        # statements executed and their timings. This consumes plenty of memory
        # and cpu time.
        tmp_debug = settings.DEBUG
        settings.DEBUG = False

        # Pick up options
        force = options["force"]
        promote = options["promote"]
        test = "FREPPLE_TEST" in os.environ
        if options["user"]:
            try:
                user = User.objects.all().get(username=options["user"])
            except Exception:
                raise CommandError("User '%s' not found" % options["user"])
        else:
            user = None

        # Synchronize the scenario table with the settings
        Scenario.syncWithSettings()

        # Initialize the task
        source = options["source"]
        try:
            sourcescenario = Scenario.objects.using(DEFAULT_DB_ALIAS).get(pk=source)
        except Exception:
            raise CommandError("No source database defined with name '%s'" % source)
        now = datetime.now()
        task = None
        if "task" in options and options["task"]:
            try:
                task = Task.objects.all().using(source).get(pk=options["task"])
            except Exception:
                raise CommandError("Task identifier not found")
            if (
                task.started
                or task.finished
                or task.status != "Waiting"
                or task.name != "scenario_copy"
            ):
                raise CommandError("Invalid task identifier")
            task.status = "0%"
            task.started = now
        else:
            task = Task(
                name="scenario_copy", submitted=now, started=now, status="0%", user=user
            )
        task.processid = os.getpid()
        task.save(using=source)

        # Validate the arguments
        destination = options["destination"]
        destinationscenario = None
        try:
            task.arguments = "%s%s %s" % (
                ("--dumpfile=%s " % options["dumpfile"]) if options["dumpfile"] else "",
                source,
                destination,
            )
            if options["description"]:
                task.arguments += '--description="%s"' % options["description"].replace(
                    '"', '\\"'
                )
            if force:
                task.arguments += " --force"
            task.save(using=source)
            try:
                destinationscenario = Scenario.objects.using(DEFAULT_DB_ALIAS).get(
                    pk=destination
                )
            except Exception:
                raise CommandError(
                    "No destination database defined with name '%s'" % destination
                )
            if source == destination:
                raise CommandError("Can't copy a schema on itself")
            if sourcescenario.status != "In use":
                raise CommandError("Source scenario is not in use")
            if destinationscenario.status != "Free" and not force and not promote:
                # make sure destination scenario is properly built otherwise it is considered Free
                scenario_is_free = False
                try:
                    User.objects.using(
                        destination
                    ).all().count()  # fails if scenario not properly built
                except Exception:
                    scenario_is_free = True
                if not scenario_is_free:
                    raise CommandError("Destination scenario is not free")
            if promote and (
                destination != DEFAULT_DB_ALIAS or source == DEFAULT_DB_ALIAS
            ):
                raise CommandError(
                    "Incorrect source or destination database with promote flag"
                )

            # check that dump file exists
            if options["dumpfile"] and not os.path.isfile(
                os.path.join(settings.FREPPLE_LOGDIR, options["dumpfile"])
            ):
                raise CommandError("Cannot find dump file %s" % options["dumpfile"])

            # Logging message - always logging in the default database
            destinationscenario.status = "Busy"
            destinationscenario.save(update_fields=["status"], using=DEFAULT_DB_ALIAS)

            # tables excluded from promotion task
            excludedTables = [
                "common_user",
                "common_scenario",
                "auth_group",
                "auth_group_permission",
                "auth_permission",
                "django_content_type",
                "common_comment",
                "common_notification",
                "common_follower",
                "common_user_groups",
                "common_attribute",
                "common_user_user_permissions",
                "common_preferences",
                "reportmanager_report",
                "reportmanager_column",
                "execute_schedule",
            ]

            # Copying the data
            # Commenting the next line is a little more secure, but requires you to create a .pgpass file.
            if not options["dumpfile"]:
                if settings.DATABASES[source]["PASSWORD"]:
                    os.environ["PGPASSWORD"] = settings.DATABASES[source]["PASSWORD"]
                if os.name == "nt":
                    # On windows restoring with pg_restore over a pipe is broken :-(
                    cmd = "pg_dump -c -Fp %s%s%s%s%s | psql %s%s%s%s"
                else:
                    cmd = "pg_dump -Fc %s%s%s%s%s | pg_restore -n public -Fc -c --if-exists %s%s%s -d %s"
                commandline = cmd % (
                    settings.DATABASES[source]["USER"]
                    and ("-U %s " % settings.DATABASES[source]["USER"])
                    or "",
                    settings.DATABASES[source]["HOST"]
                    and ("-h %s " % settings.DATABASES[source]["HOST"])
                    or "",
                    settings.DATABASES[source]["PORT"]
                    and ("-p %s " % settings.DATABASES[source]["PORT"])
                    or "",
                    ("%s " % (" -T ".join(["", *excludedTables])))
                    if destination == DEFAULT_DB_ALIAS
                    else "",
                    test
                    and settings.DATABASES[source]["TEST"]["NAME"]
                    or settings.DATABASES[source]["NAME"],
                    settings.DATABASES[destination]["USER"]
                    and ("-U %s " % settings.DATABASES[destination]["USER"])
                    or "",
                    settings.DATABASES[destination]["HOST"]
                    and ("-h %s " % settings.DATABASES[destination]["HOST"])
                    or "",
                    settings.DATABASES[destination]["PORT"]
                    and ("-p %s " % settings.DATABASES[destination]["PORT"])
                    or "",
                    test
                    and settings.DATABASES[destination]["TEST"]["NAME"]
                    or settings.DATABASES[destination]["NAME"],
                )
            else:
                cmd = "pg_restore -n public -Fc -c --if-exists --no-password %s%s%s -d %s %s"
                commandline = cmd % (
                    settings.DATABASES[destination]["USER"]
                    and ("-U %s " % settings.DATABASES[destination]["USER"])
                    or "",
                    settings.DATABASES[destination]["HOST"]
                    and ("-h %s " % settings.DATABASES[destination]["HOST"])
                    or "",
                    settings.DATABASES[destination]["PORT"]
                    and ("-p %s " % settings.DATABASES[destination]["PORT"])
                    or "",
                    test
                    and settings.DATABASES[destination]["TEST"]["NAME"]
                    or settings.DATABASES[destination]["NAME"],
                    os.path.join(settings.FREPPLE_LOGDIR, options["dumpfile"]),
                )

            with subprocess.Popen(
                commandline,
                shell=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.STDOUT,
            ) as p:
                try:
                    task.processid = p.pid
                    task.save(using=source)
                    p.wait()
                    # Successful copy can still leave warnings and errors
                    # To confirm copy is ok, let's check that the scenario copy task exists
                    # in the destination database
                    t = Task.objects.using(destination).filter(id=task.id).first()
                    if not t or t.name != task.name or t.submitted != task.submitted:
                        destinationscenario.status = "Free"
                        destinationscenario.lastrefresh = datetime.today()
                        destinationscenario.save(
                            update_fields=["status", "lastrefresh"],
                            using=DEFAULT_DB_ALIAS,
                        )
                        raise Exception("Database copy failed")
                    t.status = "Done"
                    t.finished = datetime.now()
                    t.message = "Scenario copied from %s" % source
                    t.save(
                        using=destination,
                        update_fields=["status", "finished", "message"],
                    )

                except Exception:
                    p.kill()
                    p.wait()
                    # Consider the destination database free again
                    if destination != DEFAULT_DB_ALIAS:
                        destinationscenario.status = "Free"
                        destinationscenario.lastrefresh = datetime.today()
                        destinationscenario.save(
                            update_fields=["status", "lastrefresh"],
                            using=DEFAULT_DB_ALIAS,
                        )
                    raise Exception("Database copy failed")

            # Check the permissions after restoring a backup.
            if (
                options["dumpfile"]
                and task.user
                and not User.objects.using(destination)
                .filter(username=task.user.username, is_active=True)
                .count()
            ):
                # Restoring a backup shouldn't give a user access to data he didn't have access to before...
                raise Exception(
                    "Permission denied - you did't have access rights to the scenario that was backed up"
                )

            # Update the scenario table
            destinationscenario.status = "In use"
            destinationscenario.lastrefresh = datetime.today()
            if options["description"]:
                destinationscenario.description = options["description"]
                destinationscenario.save(
                    update_fields=["status", "lastrefresh", "description"],
                    using=DEFAULT_DB_ALIAS,
                )
            else:
                destinationscenario.save(
                    update_fields=["status", "lastrefresh"], using=DEFAULT_DB_ALIAS
                )

            # Delete parameter that marks a running worker
            if destination != DEFAULT_DB_ALIAS:
                try:
                    Parameter.objects.using(destination).filter(
                        name="Worker alive"
                    ).delete()
                except BaseException:
                    pass

            # Give access to the destination scenario to:
            #  a) the user doing the copy
            #  b) all active superusers from the source schema
            # unless it's a promotion
            if destination != DEFAULT_DB_ALIAS:
                User.objects.using(destination).filter(
                    is_superuser=True, is_active=True
                ).update(is_active=True)
                User.objects.using(destination).filter(is_superuser=False).update(
                    is_active=False
                )
                if user:
                    User.objects.using(destination).filter(
                        username=user.username
                    ).update(is_active=True)

            # Delete data files present in the scenario folders
            if destination != DEFAULT_DB_ALIAS and settings.DATABASES[destination][
                "FILEUPLOADFOLDER"
            ] not in (
                settings.DATABASES[DEFAULT_DB_ALIAS]["FILEUPLOADFOLDER"],
                settings.DATABASES[source]["FILEUPLOADFOLDER"],
            ):
                FileManager.cleanFolder(0, destination)
                FileManager.cleanFolder(1, destination)

            # Logging message
            task.processid = None
            task.status = "Done"
            task.finished = datetime.now()

            # Update the task in the destination database
            dest_task = Task(
                name=task.name,
                submitted=task.submitted,
                started=task.started,
                finished=task.finished,
                arguments=task.arguments,
                status="Done",
                message=task.message,
                user=user,
            )
            if options["dumpfile"]:
                dest_task.message = "Scenario restored from %s" % options["dumpfile"]
            elif promote:
                dest_task.message = "Scenario promoted from %s" % source
            else:
                dest_task.message = "Scenario copied from %s" % source
            dest_task.save(using=destination)
            if options["dumpfile"]:
                task.message = "Scenario %s restored from %s" % (
                    destination,
                    options["dumpfile"],
                )
            else:
                task.message = "Scenario copied to %s" % destination

            # Delete any waiting tasks in the new copy.
            # This is needed for situations where the same source is copied to
            # multiple destinations at the same moment.
            if not options["dumpfile"]:
                Task.objects.all().using(destination).filter(id__gt=task.id).delete()

            # Don't automate any task in the new copy
            if not promote:
                for i in ScheduledTask.objects.all().using(destination):
                    i.next_run = None
                    i.data.pop("starttime", None)
                    i.data.pop("monday", None)
                    i.data.pop("tuesday", None)
                    i.data.pop("wednesday", None)
                    i.data.pop("thursday", None)
                    i.data.pop("friday", None)
                    i.data.pop("saturday", None)
                    i.data.pop("sunday", None)
                    i.save(using=destination)

            if options["dumpfile"]:
                setattr(_thread_locals, "database", destination)
                call_command("migrate", database=destination)
                delattr(_thread_locals, "database")
        except Exception as e:
            if task:
                task.status = "Failed"
                task.message = "%s" % e
                task.finished = datetime.now()
            if destinationscenario and destinationscenario.status == "Busy":
                if destination == DEFAULT_DB_ALIAS:
                    destinationscenario.status = "In use"
                else:
                    destinationscenario.status = "Free"
                destinationscenario.save(
                    update_fields=[
                        "status",
                    ],
                    using=DEFAULT_DB_ALIAS,
                )
            raise e

        finally:
            if task:
                task.processid = None
                task.save(using=source)
            settings.DEBUG = tmp_debug

    # accordion template
    title = _("scenario management")
    index = 1500
    help_url = "command-reference.html#scenario-copy"

    @staticmethod
    def getHTML(request):

        # Synchronize the scenario table with the settings
        Scenario.syncWithSettings()

        scenarios = Scenario.objects.using(DEFAULT_DB_ALIAS)
        if scenarios.count() <= 1:
            return None

        release_perm = []
        copy_perm = []
        promote_perm = []
        active_scenarios = []
        free_scenarios = []
        in_use_scenarios = []
        dumps = []

        default_db_not_empty = Item.objects.using(DEFAULT_DB_ALIAS).count() > 0

        # look for dump files in the log folder of production
        for f in sorted(os.listdir(settings.FREPPLE_LOGDIR)):
            if os.path.isfile(
                os.path.join(settings.FREPPLE_LOGDIR, f)
            ) and f.lower().endswith(".dump"):
                dumps.append(f)

        for scenario in scenarios:
            try:

                user = User.objects.using(scenario.name).get(
                    username=request.user.username
                )

                if scenario.status != "Free":
                    in_use_scenarios.append(scenario.name)
                else:
                    free_scenarios.append(scenario.name)

                if user.has_perm("common.release_scenario"):
                    release_perm.append(scenario.name)
                if default_db_not_empty and user.has_perm("common.promote_scenario"):
                    promote_perm.append(scenario.name)
                if user.has_perm("common.copy_scenario"):
                    copy_perm.append(scenario.name)
                if user.is_active:
                    active_scenarios.append(scenario.name)
            except Exception:
                # database schema is not properly created, scenario is free
                free_scenarios.append(scenario.name)
                active_scenarios.append(scenario.name)

        # If all scenarios are in use and user is inactive in all of them then he won't see the scenario management menu
        if len(free_scenarios) == 0 and len(active_scenarios) == 1:
            return None

        return render_to_string(
            "commands/scenario_copy.html",
            {
                "scenarios": scenarios,
                "DEFAULT_DB_ALIAS": DEFAULT_DB_ALIAS,
                "current_database": request.database,
                "release_perm": release_perm,
                "copy_perm": copy_perm,
                "promote_perm": promote_perm,
                "active_scenarios": active_scenarios,
                "free_scenarios": free_scenarios,
                "dumps": dumps,
            },
            request=request,
        )
