/***************************************************************************
 *                                                                         *
 * Copyright (C) 2007-2015 by frePPLe bv                                   *
 *                                                                         *
 * This library is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU Affero General Public License as published   *
 * by the Free Software Foundation; either version 3 of the License, or    *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This library is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
 * GNU Affero General Public License for more details.                     *
 *                                                                         *
 * You should have received a copy of the GNU Affero General Public        *
 * License along with this program.                                        *
 * If not, see <http://www.gnu.org/licenses/>.                             *
 *                                                                         *
 ***************************************************************************/

#define FREPPLE_CORE
#include "frepple/solver.h"

namespace frepple {

bool sortLoad(const Load* lhs, const Load* rhs) {
  auto l = lhs->getPriority();
  auto r = rhs->getPriority();
  if (!l) l = INT_MAX;
  if (!r) r = INT_MAX;
  if (l == r)
    return lhs->getResource()->getEfficiency() <
           rhs->getResource()->getEfficiency();
  else
    return l < r;
}

bool sortResource(const Resource* lhs, const Resource* rhs) {
  if (lhs->getEfficiency() == rhs->getEfficiency())
    return lhs->getName() < rhs->getName();
  else
    return lhs->getEfficiency() < rhs->getEfficiency();
}

void SolverCreate::chooseResource(
    const Load* l, void* v)  // @todo handle unconstrained plan!!!!
{
  SolverData* data = static_cast<SolverData*>(v);
  if ((!l->getSkill() && !l->getResource()->isGroup()) ||
      data->state->q_loadplan->getConfirmed()) {
    // CASE 1: No skill involved, and no aggregate resource either
    data->state->q_loadplan->getResource()->solve(*this, v);
    return;
  }

  // CASE 2: Skill involved, or aggregate resource
  short loglevel = getLogLevel();

  // Control the planning mode
  bool originalPlanningMode = data->constrainedPlanning;
  data->constrainedPlanning = true;

  // Don't keep track of the constraints right now
  bool originalLogConstraints = data->logConstraints;
  data->logConstraints = false;

  // Initialize
  Date min_next_date(Date::infiniteFuture);
  LoadPlan* lplan = data->state->q_loadplan;
  Resource* bestAlternateSelection = nullptr;
  OperationPlanState bestAlternateState, firstAlternateState;
  Resource* firstAlternate = nullptr;
  bool qualified_resource_exists = false;
  double bestAlternateValue = DBL_MAX;
  double bestAlternateQuantity = DBL_MIN;
  double beforeCost = data->state->a_cost;
  double beforePenalty = data->state->a_penalty;
  OperationPlanState originalOpplan(lplan->getOperationPlan());
  double originalLoadplanQuantity = lplan->getQuantity();
  setLogLevel(0);  // Silence during this loop

  // Create flow and loadplans
  if (lplan->getOperationPlan()->beginLoadPlans() ==
      lplan->getOperationPlan()->endLoadPlans())
    lplan->getOperationPlan()->createFlowLoads();

  // Build a list of candidate resources
  vector<Resource*> res_stack;
  if (l->getResource()->isGroup()) {
    for (auto c1 = l->getResource()->getMembers(); c1 != Resource::end();
         ++c1) {
      if (c1->isGroup()) {
        for (auto c2 = c1->getMembers(); c2 != Resource::end(); ++c2) {
          if (c2->isGroup()) {
            for (auto c3 = c2->getMembers(); c3 != Resource::end(); ++c3) {
              if (c3->isGroup()) {
                for (auto c4 = c3->getMembers(); c4 != Resource::end(); ++c4) {
                  if (c4->isGroup()) {
                    for (auto c5 = c4->getMembers(); c5 != Resource::end();
                         ++c5) {
                      if (c5->isGroup())
                        logger << "Warning: Resource "
                                  "hierarchies can only have up to 5 levels"
                               << endl;
                      else
                        res_stack.push_back(&*c5);
                    }
                  } else
                    res_stack.push_back(&*c4);
                }
              } else
                res_stack.push_back(&*c3);
            }
          } else
            res_stack.push_back(&*c2);
        }
      } else
        res_stack.push_back(&*c1);
    }
    // Sort the list by efficiciency and name
    sort(res_stack.begin(), res_stack.end(), sortResource);
  } else
    res_stack.push_back(l->getResource());

  // Loop over all candidate resources
  while (!res_stack.empty()) {
    // Pick next resource
    Resource* res = res_stack.back();
    res_stack.pop_back();

    // Check if the resource has the right skill
    ResourceSkill* rscSkill = nullptr;
    if (l->getSkill() && !res->hasSkill(l->getSkill(), originalOpplan.start,
                                        originalOpplan.end, &rscSkill))
      continue;
    // TODO if there is a date effective skill, we need to consider it in the
    // reply
    qualified_resource_exists = true;

    // Avoid double allocations to the same resource
    if (lplan->getLoad()->getResource()->isGroup() &&
        Plan::instance().getIndividualPoolResources()) {
      bool exists = false;
      for (auto g = lplan->getOperationPlan()->getLoadPlans();
           g != lplan->getOperationPlan()->endLoadPlans() && &*g != lplan &&
           g->getQuantity() < 0.0;
           ++g) {
        if (g->getResource() == res) {
          exists = true;
          break;
        }
      }
      if (exists) continue;
    }

    // Switch to this resource
    data->state->q_loadplan = lplan;  // because q_loadplan can change!
    lplan->getOperationPlan()->setStartEndAndQuantity(
        originalOpplan.start, originalOpplan.end, originalOpplan.quantity);
    lplan->setResource(res, false, false);
    lplan->getOperationPlan()->setEnd(originalOpplan.end);
    data->state->q_qty = lplan->getQuantity();
    data->state->q_date = lplan->getDate();

    // Remember the first alternate
    if (!firstAlternate) {
      firstAlternate = res;
      firstAlternateState = lplan->getOperationPlan();
    }

    // Plan the resource
    auto topcommand = data->getCommandManager()->setBookmark();
    try {
      res->solve(*this, data);
    } catch (...) {
      setLogLevel(loglevel);
      data->constrainedPlanning = originalPlanningMode;
      data->logConstraints = originalLogConstraints;
      data->getCommandManager()->rollback(topcommand);
      throw;
    }
    data->getCommandManager()->rollback(topcommand);

    // Evaluate the result
    if (data->state->a_qty > ROUNDING_ERROR &&
        lplan->getOperationPlan()->getQuantity() > 0) {
      double deltaCost = data->state->a_cost - beforeCost;
      double deltaPenalty = data->state->a_penalty - beforePenalty;
      // Message
      if (loglevel > 1)
        logger << indentlevel << "Operation '" << l->getOperation()
               << "' evaluates alternate '" << res << "': cost " << deltaCost
               << ", penalty " << deltaPenalty << endl;
      data->state->a_cost = beforeCost;
      data->state->a_penalty = beforePenalty;
      double val = 0.0;
      switch (l->getSearch()) {
        case SearchMode::PRIORITY:
          val = rscSkill ? rscSkill->getPriority() : 0;
          break;
        case SearchMode::MINCOST:
          val = deltaCost / lplan->getOperationPlan()->getQuantity();
          break;
        case SearchMode::MINPENALTY:
          val = deltaPenalty / lplan->getOperationPlan()->getQuantity();
          break;
        case SearchMode::MINCOSTPENALTY:
          val = (deltaCost + deltaPenalty) /
                lplan->getOperationPlan()->getQuantity();
          break;
        default:
          throw LogicException("Unsupported search mode for alternate load");
      }
      if (val + ROUNDING_ERROR < bestAlternateValue ||
          (fabs(val - bestAlternateValue) < ROUNDING_ERROR &&
           lplan->getOperationPlan()->getQuantity() > bestAlternateQuantity)) {
        // Found a better alternate
        bestAlternateValue = val;
        bestAlternateSelection = res;
        bestAlternateState = OperationPlanState(lplan->getOperationPlan());
        bestAlternateQuantity = lplan->getOperationPlan()->getQuantity();
      }
    } else if (loglevel > 1)
      logger << indentlevel << "Operation '" << l->getOperation()
             << "' evaluates alternate '" << lplan->getResource()
             << "': not available before " << data->state->a_date << endl;

    // Keep track of best next date
    if (data->state->a_date < min_next_date)
      min_next_date = data->state->a_date;
  }
  setLogLevel(loglevel);

  // Not a single resource has the appropriate skills. You're joking?
  if (!qualified_resource_exists) {
    stringstream s;
    s << "No subresource of '" << l->getResource() << "' has the skill '"
      << l->getSkill() << "' required for operation '" << l->getOperation()
      << "'";
    throw DataException(s.str());
  }

  // Restore the best candidate we found in the loop above
  if (bestAlternateSelection) {
    // Message
    if (loglevel > 1)
      logger << indentlevel << "  Operation '" << l->getOperation()
             << "' chooses alternate '" << bestAlternateSelection << "' "
             << l->getSearch() << endl;

    // Switch back
    data->state->q_loadplan = lplan;  // because q_loadplan can change!
    data->state->a_cost = beforeCost;
    data->state->a_penalty = beforePenalty;

    if (lplan->getResource() != bestAlternateSelection) {
      lplan->getOperationPlan()->clearSetupEvent();
      lplan->getOperationPlan()->setStartEndAndQuantity(
          bestAlternateState.start, bestAlternateState.end,
          bestAlternateState.quantity);
      lplan->setResource(bestAlternateSelection, false, false);
    }
    data->state->q_qty = lplan->getQuantity();
    data->state->q_date = lplan->getDate();
    bestAlternateSelection->solve(*this, data);

    // Restore the planning mode
    data->constrainedPlanning = originalPlanningMode;
    data->logConstraints = originalLogConstraints;
    return;
  }

  if (!originalPlanningMode) {
    // No alternate gave a good result in an unconstrained plan
    if (lplan->getResource() != firstAlternate ||
        !lplan->getOperationPlan()->getQuantity()) {
      lplan->getOperationPlan()->clearSetupEvent();
      lplan->getOperationPlan()->setStartEndAndQuantity(
          firstAlternateState.start, firstAlternateState.end,
          firstAlternateState.quantity);
      lplan->setResource(firstAlternate, false, false);
    }
    data->state->a_qty = lplan->getQuantity();
    data->state->a_date = lplan->getDate();

    // Restore the planning mode
    data->constrainedPlanning = originalPlanningMode;
    data->logConstraints = originalLogConstraints;

    if (loglevel > 1)
      logger << indentlevel << "Alternate load overloads alternate "
             << firstAlternate << endl;
  } else {
    // No alternate gave a good result in a constrained plan
    data->state->a_date = min_next_date;
    data->state->a_qty = 0;

    // Maintain the constraint list
    if (originalLogConstraints && data->constraints)
      data->constraints->push(ProblemCapacityOverload::metadata,
                              l->getResource(), originalOpplan.start,
                              originalOpplan.end, -originalLoadplanQuantity);

    // Restore the planning mode
    data->constrainedPlanning = originalPlanningMode;
    data->logConstraints = originalLogConstraints;

    if (loglevel > 1)
      logger << indentlevel
             << "  Alternate load doesn't find supply on any alternate: "
             << "not available before " << data->state->a_date << endl;
  }
}

void SolverCreate::solve(const Load* l, void* v) {
  // Note: This method is only called for decrease loadplans and for the leading
  // load of an alternate group. See SolverCreate::checkOperation
  SolverData* data = static_cast<SolverData*>(v);

  if ((!l->hasAlternates() && !l->getAlternate()) ||
      data->state->q_loadplan->getConfirmed()) {
    // CASE I: It is not an alternate load.
    // Delegate the answer immediately to the resource
    chooseResource(l, data);
    return;
  }

  // CASE II: It is an alternate load.
  // We ask each alternate load in order of priority till we find a load
  // that has a non-zero reply.
  short loglevel = getLogLevel();

  // 1) collect a list of alternates
  list<const Load*> thealternates;
  const Load* x = l->hasAlternates() ? l : l->getAlternate();
  SearchMode search = l->getSearch();
  for (auto i = l->getOperation()->getLoads().begin();
       i != l->getOperation()->getLoads().end(); ++i)
    if ((i->getAlternate() == x || &*i == x) && i->getPriority() &&
        i->getEffective().within(data->state->q_loadplan->getDate()))
      thealternates.push_back(&*i);

  // 2) Sort the list
  thealternates.sort(sortLoad);  // @todo cpu-intensive - better is to maintain
                                 // the list in the correct order

  // 3) Control the planning mode
  bool originalPlanningMode = data->constrainedPlanning;
  data->constrainedPlanning = true;

  // Don't keep track of the constraints right now
  bool originalLogConstraints = data->logConstraints;
  data->logConstraints = false;

  // 4) Loop through all alternates or till we find a non-zero
  // reply (priority search)
  Date min_next_date(Date::infiniteFuture);
  LoadPlan* lplan = data->state->q_loadplan;
  double bestAlternateValue = DBL_MAX;
  double bestAlternateQuantity = DBL_MIN;
  const Load* bestAlternateSelection = nullptr;
  double beforeCost = data->state->a_cost;
  double beforePenalty = data->state->a_penalty;
  OperationPlanState originalOpplan(lplan->getOperationPlan());
  double originalLoadplanQuantity = data->state->q_loadplan->getQuantity();
  for (auto i = thealternates.begin(); i != thealternates.end();) {
    const Load* curload = *i;
    data->state->q_loadplan = lplan;  // because q_loadplan can change!

    // 4a) Switch to this load
    if (lplan->getLoad() != curload) lplan->setLoad(const_cast<Load*>(curload));
    lplan->getOperationPlan()->setQuantity(originalOpplan.quantity);
    lplan->getOperationPlan()->setEnd(originalOpplan.end);
    data->state->q_qty = lplan->getQuantity();
    data->state->q_date = lplan->getDate();

    // 4b) Ask the resource
    // TODO XXX Need to insert another loop here! It goes over all resources
    // qualified for the required skill. The qualified resources need to be
    // sorted based on their cost. If the cost is the same we should use a
    // decent tie breaker, eg number of skills or number of loads. The first
    // resource with the qualified skill that is available will be used.
    auto topcommand = data->getCommandManager()->setBookmark();
    if (search == SearchMode::PRIORITY)
      curload->getResource()->solve(*this, data);
    else {
      setLogLevel(0);
      try {
        curload->getResource()->solve(*this, data);
      } catch (...) {
        setLogLevel(loglevel);
        // Restore the planning mode
        data->constrainedPlanning = originalPlanningMode;
        data->logConstraints = originalLogConstraints;
        throw;
      }
      setLogLevel(loglevel);
    }

    // 4c) Evaluate the result
    if (data->state->a_qty > ROUNDING_ERROR &&
        lplan->getOperationPlan()->getQuantity() > 0) {
      if (search == SearchMode::PRIORITY) {
        // Priority search: accept any non-zero reply
        // Restore the planning mode
        data->constrainedPlanning = originalPlanningMode;
        data->logConstraints = originalLogConstraints;
        return;
      } else {
        // Other search modes: evaluate all
        double deltaCost = data->state->a_cost - beforeCost;
        double deltaPenalty = data->state->a_penalty - beforePenalty;
        // Message
        if (loglevel > 1 && search != SearchMode::PRIORITY)
          logger << indentlevel << "Operation '" << l->getOperation()
                 << "' evaluates alternate '" << curload->getResource()
                 << "': cost " << deltaCost << ", penalty " << deltaPenalty
                 << endl;
        if (deltaCost < ROUNDING_ERROR && deltaPenalty < ROUNDING_ERROR) {
          // Zero cost and zero penalty on this alternate. It won't get any
          // better than this, so we accept this alternate.
          if (loglevel > 1)
            logger << indentlevel << "Operation '" << l->getOperation()
                   << "' chooses alternate '" << curload->getResource() << "' "
                   << search << endl;
          // Restore the planning mode
          data->constrainedPlanning = originalPlanningMode;
          data->logConstraints = originalLogConstraints;
          return;
        }
        data->state->a_cost = beforeCost;
        data->state->a_penalty = beforePenalty;
        double val = 0.0;
        switch (search) {
          case SearchMode::MINCOST:
            val = deltaCost / lplan->getOperationPlan()->getQuantity();
            break;
          case SearchMode::MINPENALTY:
            val = deltaPenalty / lplan->getOperationPlan()->getQuantity();
            break;
          case SearchMode::MINCOSTPENALTY:
            val = (deltaCost + deltaPenalty) /
                  lplan->getOperationPlan()->getQuantity();
            break;
          default:
            throw LogicException("Unsupported search mode for alternate load");
        }
        if (val + ROUNDING_ERROR < bestAlternateValue ||
            (fabs(val - bestAlternateValue) < ROUNDING_ERROR &&
             lplan->getOperationPlan()->getQuantity() >
                 bestAlternateQuantity)) {
          // Found a better alternate
          bestAlternateValue = val;
          bestAlternateSelection = curload;
          bestAlternateQuantity = lplan->getOperationPlan()->getQuantity();
        }
      }
    } else if (loglevel > 1 && search != SearchMode::PRIORITY)
      logger << indentlevel << "Operation '" << l->getOperation()
             << "' evaluates alternate '" << curload->getResource()
             << "': not available before " << data->state->a_date << endl;

    // 4d) Undo the plan on the alternate
    data->getCommandManager()->rollback(topcommand);

    // 4e) Prepare for the next alternate
    if (data->state->a_date < min_next_date)
      min_next_date = data->state->a_date;
    if (++i != thealternates.end() && loglevel > 1 &&
        search == SearchMode::PRIORITY)
      logger << indentlevel << "  Alternate load switches from '"
             << curload->getResource() << "' to '" << (*i)->getResource() << "'"
             << endl;
  }

  // 5) Unconstrained plan: plan on the first alternate
  if (!originalPlanningMode &&
      !(search != SearchMode::PRIORITY && bestAlternateSelection)) {
    // Switch to unconstrained planning
    data->constrainedPlanning = false;
    bestAlternateSelection = *(thealternates.begin());
  }

  // 6) Finally replan on the best alternate
  if (!originalPlanningMode ||
      (search != SearchMode::PRIORITY && bestAlternateSelection)) {
    // Message
    if (loglevel > 1)
      logger << indentlevel << "  Operation '" << l->getOperation()
             << "' chooses alternate '" << bestAlternateSelection->getResource()
             << "' " << search << endl;

    // Switch back
    data->state->q_loadplan = lplan;  // because q_loadplan can change!
    data->state->a_cost = beforeCost;
    data->state->a_penalty = beforePenalty;
    if (lplan->getLoad() != bestAlternateSelection)
      lplan->setLoad(const_cast<Load*>(bestAlternateSelection));
    lplan->getOperationPlan()->restore(originalOpplan);
    // TODO XXX need to restore also the selected resource with the right skill!
    data->state->q_qty = lplan->getQuantity();
    data->state->q_date = lplan->getDate();
    bestAlternateSelection->getResource()->solve(*this, data);

    // Restore the planning mode
    data->constrainedPlanning = originalPlanningMode;
    data->logConstraints = originalLogConstraints;
    return;
  }

  // 7) No alternate gave a good result
  data->state->a_date = min_next_date;
  data->state->a_qty = 0;

  // Restore the planning mode
  data->constrainedPlanning = originalPlanningMode;

  // Maintain the constraint list
  if (originalLogConstraints && data->constraints) {
    const Load* primary = *(thealternates.begin());
    data->constraints->push(ProblemCapacityOverload::metadata,
                            primary->getResource(), originalOpplan.start,
                            originalOpplan.end, -originalLoadplanQuantity);
  }
  data->logConstraints = originalLogConstraints;

  if (loglevel > 1)
    logger << indentlevel
           << "  Alternate load doesn't find supply on any alternate: "
           << "not available before " << data->state->a_date << endl;
}

}  // namespace frepple
