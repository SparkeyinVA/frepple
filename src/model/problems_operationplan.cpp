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
#include "frepple/model.h"
namespace frepple {

void Operation::updateProblems() {
  // Find all operationplans, and delegate the problem detection to them
  if (getDetectProblems())
    for (auto o = first_opplan; o; o = o->next) o->updateProblems();
}

//
// BEFORECURRENT, BEFOREFENCE, PRECEDENCE
//

void OperationPlan::updateProblems() {
  // A flag for each problem type that may need to be created
  bool needsBeforeCurrent(false);
  bool needsBeforeFence(false);
  bool needsPrecedence(false);

  if (!firstsubopplan) {
    // Avoid duplicating problems on child and owner operationplans
    // Check if a BeforeCurrent or BeforeFence problem is required.
    // Note that we either detect of beforeCurrent or a beforeFence problem,
    // never both simultaneously.
    if (getConfirmed()) {
      if (getEnd() < Plan::instance().getCurrent()) needsBeforeCurrent = true;
    } else {
      if (getStart() < Plan::instance().getCurrent())
        needsBeforeCurrent = true;
      else if (getProposed() && getStart() < oper->getFence(this))
        needsBeforeFence = true;
    }
  }
  // Note: 1 second grace period for precedence problems to avoid rounding
  // issues
  if (nextsubopplan && getEnd() > nextsubopplan->getStart() + Duration(1L) &&
      !nextsubopplan->getConfirmed() && owner &&
      !owner->getOperation()->hasType<OperationSplit>())
    needsPrecedence = true;

  // Loop through the existing problems
  for (auto j = Problem::begin(this, false); j != Problem::end();) {
    // Need to increment now and define a pointer to the problem, since the
    // problem can be deleted soon (which invalidates the iterator).
    Problem& curprob = *j;
    ++j;
    // The if-statement keeps the problem detection code concise and
    // concentrated. However, a drawback of this design is that a new problem
    // subclass will also require a new demand subclass. I think such a link
    // is acceptable.
    if (typeid(curprob) == typeid(ProblemBeforeCurrent)) {
      if (needsBeforeCurrent)
        needsBeforeCurrent = false;
      else if (this == curprob.getOwner())
        delete &curprob;
    } else if (typeid(curprob) == typeid(ProblemBeforeFence)) {
      if (needsBeforeFence)
        needsBeforeFence = false;
      else if (this == curprob.getOwner())
        delete &curprob;
    } else if (typeid(curprob) == typeid(ProblemPrecedence)) {
      if (needsPrecedence)
        needsPrecedence = false;
      else if (this == curprob.getOwner())
        delete &curprob;
    }
  }

  // Create the problems that are required but aren't existing yet.
  if (needsBeforeCurrent) new ProblemBeforeCurrent(this);
  if (needsBeforeFence) new ProblemBeforeFence(this);
  if (needsPrecedence) new ProblemPrecedence(this);
}

OperationPlan::ProblemIterator::ProblemIterator(const OperationPlan* o,
                                                bool include_related)
    : Problem::iterator(o->firstProblem), opplan(o) {
  if (!include_related) return;

  // Adding related capacity problems
  for (auto ldpln = opplan->beginLoadPlans(); ldpln != opplan->endLoadPlans();
       ++ldpln) {
    if (!ldpln->getResource()->getConstrained()) continue;
    auto prob_iter = ldpln->getResource()->getProblems();
    if (ldpln->getResource()->hasType<ResourceBuckets>()) {
      while (Problem* prob = prob_iter.next()) {
        if (prob->getDates().within(opplan->getStart()) &&
            !prob->isFeasible()) {
          relatedproblems.push_back(&*prob);
          break;
        }
      }
    } else {
      while (Problem* prob = prob_iter.next()) {
        if (prob->getDates().overlap(opplan->getDates()) &&
            !prob->isFeasible()) {
          relatedproblems.push_back(&*prob);
          break;
        }
      }
    }
  }

  // Adding local and upstream material problems
  for (PeggingIterator p(o, false); p; --p) {
    const OperationPlan* m = p.getOperationPlan();
    if (m->getCompleted() || m->getClosed()) continue;
    if (m != o) {
      ProblemIterator probs(m, false);
      while (auto p = probs.next()) {
        if (p->isFeasible() ||
            !p->hasType<ProblemBeforeCurrent, ProblemBeforeFence,
                        ProblemPrecedence>())
          continue;
        bool exists = false;
        for (auto& x : relatedproblems)
          if (static_cast<OperationPlan*>(x->getOwner())->getOperation() ==
              static_cast<OperationPlan*>(p->getOwner())->getOperation()) {
            exists = true;
            break;
          }
        if (!exists) relatedproblems.push_back(&*p);
      }
    }
    for (auto fp = m->beginFlowPlans(); fp != m->endFlowPlans(); ++fp) {
      if (fp->getOnhandAfterDate() < -ROUNDING_ERROR)
        for (Problem::iterator prob(fp->getBuffer()); prob != Problem::end();
             ++prob) {
          if (prob->isFeasible()) continue;
          if (prob->getDates().overlap(m->getDates()) && !prob->isFeasible()) {
            bool exists = false;
            for (auto& x : relatedproblems)
              if (x->getOwner() == prob->getOwner()) {
                exists = true;
                break;
              }
            if (!exists) relatedproblems.push_back(&*prob);
            break;
          }
        }
    }
  }

  // Update the first problem pointer
  if (!relatedproblems.empty()) iter = relatedproblems.back();
}

OperationPlan::ProblemIterator& OperationPlan::ProblemIterator::operator++() {
  // Incrementing beyond the end
  if (!iter) return *this;

  if (!relatedproblems.empty()) {
    relatedproblems.pop_back();
    if (relatedproblems.empty())
      iter = opplan->firstProblem;
    else
      iter = relatedproblems.back();
    return *this;
  }

  // Move to the next problem
  iter = iter->getNextProblem();
  return *this;
}

bool OperationPlan::updateFeasible() {
  if (!getOperation()->getDetectProblems() || getCompleted() || getClosed()) {
    // No problems to be flagged on this operation
    setFeasible(true);
    return true;
  }

  // The implementation of this method isn't really cleanly object oriented. It
  // uses logic which only the different resource and buffer implementation
  // classes should be aware.
  if (firstsubopplan) {
    // Check feasibility of child operationplans
    for (auto i = firstsubopplan; i; i = i->nextsubopplan) {
      if (!i->updateFeasible()) {
        setFeasible(false);
        return false;
      }
    }
  } else {
    // Before current and before fence problems are only detected on child
    // operationplans
    if (getConfirmed()) {
      if (getEnd() < Plan::instance().getCurrent()) {
        // Before current violation
        setFeasible(false);
        return false;
      }
    } else {
      if (getStart() < Plan::instance().getCurrent()) {
        // Before current violation
        setFeasible(false);
        return false;
      } else if (getProposed() && getStart() < oper->getFence(this)) {
        // Before fence violation
        setFeasible(false);
        return false;
      }
    }
  }
  if (nextsubopplan && getEnd() > nextsubopplan->getStart() + Duration(1L) &&
      !nextsubopplan->getConfirmed() && owner &&
      !owner->getOperation()->hasType<OperationSplit>()) {
    // Precedence violation
    // Note: 1 second grace period for precedence problems to avoid rounding
    // issues
    setFeasible(false);
    return false;
  }

  // Verify the capacity constraints
  for (auto ldplan = getLoadPlans(); ldplan != endLoadPlans(); ++ldplan) {
    if (((ldplan->getQuantity() > 0 &&
          ldplan->getResource()->hasType<ResourceDefault>()) ||
         (ldplan->getQuantity() < 0 &&
          ldplan->getResource()->hasType<ResourceBuckets>())) &&
        !ldplan->getFeasible()) {
      setFeasible(false);
      return false;
    }
  }

  // Verify the material constraints
  for (auto flplan = beginFlowPlans(); flplan != endFlowPlans(); ++flplan) {
    if (!flplan->getFlow()->isConsumer() ||
        flplan->getBuffer()->hasType<BufferInfinite>())
      continue;
    if (flplan->getBuffer()->getOnHand(Date::infiniteFuture) <
        -ROUNDING_ERROR) {
      // Material shortage
      setFeasible(false);
      return false;
    }
    auto flplaniter = flplan->getBuffer()->getFlowPlans();
    for (auto cur = flplaniter.begin(&*flplan);
         cur != flplaniter.end() && cur->getDate() < getEnd(); ++cur) {
      if (cur->getOnhand() < -ROUNDING_ERROR && cur->isLastOnDate()) {
        // Material shortage
        setFeasible(false);
        return false;
      }
    }
  }

  // After all checks, it turns out to be feasible
  setFeasible(true);
  return true;
}

PyObject* OperationPlan::updateFeasiblePython(PyObject* self, PyObject* args) {
  return PythonData(static_cast<OperationPlan*>(self)->updateFeasible());
}

}  // namespace frepple
