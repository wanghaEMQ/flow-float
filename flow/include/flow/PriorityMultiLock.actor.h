/*
 * PriorityMultiLock.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// When actually compiled (NO_INTELLISENSE), include the generated version of this file.  In intellisense use the source
// version.
#if defined(NO_INTELLISENSE) && !defined(FLOW_PRIORITYMULTILOCK_ACTOR_G_H)
#define FLOW_PRIORITYMULTILOCK_ACTOR_G_H
#include "flow/PriorityMultiLock.actor.g.h"
#elif !defined(PRIORITYMULTILOCK_ACTOR_H)
#define PRIORITYMULTILOCK_ACTOR_H

#include "flow/flow.h"
#include <boost/intrusive/list.hpp>
#include "flow/actorcompiler.h" // This must be the last #include.

#define PRIORITYMULTILOCK_DEBUG 0

#if PRIORITYMULTILOCK_DEBUG || !defined(NO_INTELLISENSE)
#define pml_debug_printf(...)                                                                                          \
	if (now() > 0) {                                                                                                   \
		printf("pml line=%04d ", __LINE__);                                                                            \
		printf(__VA_ARGS__);                                                                                           \
	}
#else
#define pml_debug_printf(...)
#endif

// A multi user lock with a concurrent holder limit where waiters request a lock with a priority
// id and are granted locks based on a total concurrency and relative weights of the current active
// priorities.  Priority id's must start at 0 and are sequential integers.  Priority id numbers
// are not related to the importance of the priority in execution.
//
// Scheduling logic
// Let
// 	 weights[n] = configured weight for priority n
//   waiters[n] = the number of waiters for priority n
//   runnerCounts[n] = number of runners at priority n
//
//   totalPendingWeights = sum of weights for all priorities with waiters[n] > 0
//   When waiters[n] becomes == 0, totalPendingWeights -= weights[n]
//   When waiters[n] becomes  > 0, totalPendingWeights += weights[n]
//
//   The total capacity of a priority to be considered when launching tasks is
//     ceil(weights[n] / totalPendingWeights * concurrency)
//
// For improved memory locality the properties mentioned above are stored as priorities[n].<property>
// in the actual implementation.
//
// The interface is similar to FlowMutex except that lock holders can just drop the lock to release it.
//
// Usage:
//   Lock lock = wait(prioritylock.lock(priority_id));
//   lock.release();  // Explicit release, or
//   // let lock and all copies of lock go out of scope to release
class PriorityMultiLock : public ReferenceCounted<PriorityMultiLock> {
public:
	// Waiting on the lock returns a Lock, which is really just a Promise<Void>
	// Calling release() is not necessary, it exists in case the Lock holder wants to explicitly release
	// the Lock before it goes out of scope.
	struct Lock {
		void release() { promise.send(Void()); }
		bool isLocked() const { return promise.canBeSet(); }

		// This is exposed in case the caller wants to use/copy it directly
		Promise<Void> promise;
	};

	PriorityMultiLock(int concurrency, std::string weights)
	  : PriorityMultiLock(concurrency, parseStringToVector<int>(weights, ',')) {}

	PriorityMultiLock(int concurrency, std::vector<int> weightsByPriority)
	  : concurrency(concurrency), available(concurrency), waiting(0), totalPendingWeights(0), killed(false) {

		priorities.resize(weightsByPriority.size());
		for (int i = 0; i < priorities.size(); ++i) {
			priorities[i].priority = i;
			priorities[i].weight = weightsByPriority[i];
		}

		fRunner = runner(this);
	}

	~PriorityMultiLock() { kill(); }

	Future<Lock> lock(int priority = 0) {
		if (killed)
			throw broken_promise();

		Priority& p = priorities[priority];
		Queue& q = p.queue;

		// If this priority currently has no waiters
		if (q.empty()) {
			// Add this priority's weight to the total for priorities with pending work.  This must be done
			// so that currenctCapacity() below will assign capacaity to this priority.
			totalPendingWeights += p.weight;

			// If there are slots available and the priority has capacity then don't make the caller wait
			if (available > 0 && p.runners < currentCapacity(p.weight)) {
				// Remove this priority's weight from the total since it will remain empty
				totalPendingWeights -= p.weight;

				// Return a Lock to the caller
				Lock lock;
				addRunner(lock, &p);

				pml_debug_printf("lock nowait priority %d  %s\n", priority, toString().c_str());
				return lock;
			}

			// If we didn't return above then add the priority to the waitingPriorities list
			waitingPriorities.push_back(p);
		}

		Waiter& w = q.emplace_back();
		++waiting;

		pml_debug_printf("lock wait priority %d  %s\n", priority, toString().c_str());
		return w.lockPromise.getFuture();
	}

	// Halt stops the PML from handing out any new locks but leaves waiters and runners alone.
	// Existing and new waiters will not see an error, they will just never get a lock.
	// Can be safely called multiple times.
	void halt() {
		pml_debug_printf("halt %s\n", toString().c_str());
		brokenOnDestruct.reset();

		if (fRunner.isValid()) {
			fRunner.cancel();
			// Adjust available and concurrency so that if all runners finish the available
			available -= concurrency;
			concurrency = 0;
		}

		waitingPriorities.clear();
	}

	// Halt, then make existing and new waiters get a broken_promise error.
	// Can be safely called multiple times.
	void kill() {
		if (!killed) {
			// Killed must be set first because waiters which ignore exceptions could call wait again immediately.
			killed = true;
			halt();
			for (auto& p : priorities) {
				p.queue.clear();
			}
		}
	}

	std::string toString() const {
		std::string s = format("{ ptr=%p concurrency=%d available=%d running=%d waiting=%d "
		                       "pendingWeights=%d ",
		                       this,
		                       concurrency,
		                       available,
		                       concurrency - available,
		                       waiting,
		                       totalPendingWeights);

		for (auto& p : priorities) {
			s += format("{%s} ", p.toString(this).c_str());
		}

		s += "}";

		return s;
	}

	int maxPriority() const { return priorities.size() - 1; }

	int getRunnersCount() const { return concurrency - available; }
	int getWaitersCount() const { return waiting; }

	int getWaitersCount(const unsigned int priority) const {
		ASSERT(priority < priorities.size());
		return priorities[priority].queue.size();
	}

	int getRunnersCount(const unsigned int priority) const {
		ASSERT(priority < priorities.size());
		return priorities[priority].runners;
	}

private:
	struct Waiter {
		Promise<Lock> lockPromise;
	};

	// Total execution slots allowed across all priorities
	int concurrency;
	// Current available execution slots
	int available;
	// Total waiters across all priorities
	int waiting;
	// Sum of weights for all priorities with 1 or more waiters
	int totalPendingWeights;

	typedef Deque<Waiter> Queue;

	struct Priority : boost::intrusive::list_base_hook<> {
		Priority() : runners(0), weight(0), priority(-1) {}

		// Queue of waiters at this priority
		Queue queue;
		// Number of runners at this priority
		int runners;
		// Configured weight for this priority
		int weight;
		// Priority number for convenience, matches *this's index in PML priorities vector
		int priority;

		std::string toString(const PriorityMultiLock* pml) const {
			return format("priority=%d weight=%d run=%d wait=%d cap=%d",
			              priority,
			              weight,
			              runners,
			              queue.size(),
			              queue.empty() ? 0 : pml->currentCapacity(weight));
		}
	};

	std::vector<Priority> priorities;
	typedef boost::intrusive::list<Priority, boost::intrusive::constant_time_size<false>> WaitingPrioritiesList;

	// List of all priorities with 1 or more waiters.  This list exists so that the scheduling loop
	// does not have to iterage over the priorities vector checking priorities without waiters.
	WaitingPrioritiesList waitingPriorities;

	Future<Void> fRunner;
	AsyncTrigger wakeRunner;
	Promise<Void> brokenOnDestruct;
	bool killed;

	ACTOR static void handleRelease(Reference<PriorityMultiLock> self, Priority* priority, Future<Void> holder) {
		pml_debug_printf("%f handleRelease self=%p start\n", now(), self.getPtr());
		try {
			wait(holder);
			pml_debug_printf("%f handleRelease self=%p success\n", now(), self.getPtr());
		} catch (Error& e) {
			pml_debug_printf("%f handleRelease self=%p error %s\n", now(), self.getPtr(), e.what());
		}

		pml_debug_printf("lock release priority %d  %s\n", (int)(priority->priority), self->toString().c_str());

		pml_debug_printf("%f handleRelease self=%p releasing\n", now(), self.getPtr());
		++self->available;
		priority->runners -= 1;

		// If there are any waiters or if the runners array is getting large, trigger the runner loop
		if (self->waiting > 0) {
			self->wakeRunner.trigger();
		}
	}

	void addRunner(Lock& lock, Priority* priority) {
		priority->runners += 1;
		--available;
		handleRelease(Reference<PriorityMultiLock>::addRef(this), priority, lock.promise.getFuture());
	}

	// Current maximum running tasks for the specified priority, which must have waiters
	// or the result is undefined
	int currentCapacity(int weight) const {
		// The total concurrency allowed for this priority at present is the total concurrency times
		// priority's weight divided by the total weights for all priorities with waiters.
		return ceil((float)weight / totalPendingWeights * concurrency);
	}

	ACTOR static Future<Void> runner(PriorityMultiLock* self) {
		state Future<Void> error = self->brokenOnDestruct.getFuture();

		// Priority to try to run tasks from next
		state WaitingPrioritiesList::iterator p = self->waitingPriorities.end();

		loop {
			pml_debug_printf("runner loop start  priority=%d  %s\n", p->priority, self->toString().c_str());

			// Wait for a runner to release its lock
			pml_debug_printf("runner loop waitTrigger  priority=%d  %s\n", p->priority, self->toString().c_str());
			wait(self->wakeRunner.onTrigger());
			pml_debug_printf("%f runner loop wake  priority=%d  %s\n", now(), p->priority, self->toString().c_str());

			// While there are available slots and there are waiters, launch tasks
			while (self->available > 0 && self->waiting > 0) {
				pml_debug_printf("  launch loop start  priority=%d  %s\n", p->priority, self->toString().c_str());

				// Find the next priority with waiters and capacity.  There must be at least one.
				loop {
					if (p == self->waitingPriorities.end()) {
						p = self->waitingPriorities.begin();
					}

					pml_debug_printf("    launch loop scan  priority=%d  %s\n", p->priority, self->toString().c_str());

					if (!p->queue.empty() && p->runners < self->currentCapacity(p->weight)) {
						break;
					}
					++p;
				}

				Queue& queue = p->queue;
				Waiter w = queue.front();
				queue.pop_front();

				// If this priority is now empty, subtract its weight from the total pending weights an remove it
				// from the waitingPriorities list
				Priority* pPriority = &*p;
				if (queue.empty()) {
					p = self->waitingPriorities.erase(p);
					self->totalPendingWeights -= pPriority->weight;

					pml_debug_printf(
					    "      emptied priority  priority=%d  %s\n", pPriority->priority, self->toString().c_str());
				}

				--self->waiting;
				Lock lock;

				w.lockPromise.send(lock);

				// Self may have been destructed during the lock callback
				if (error.isReady()) {
					throw error.getError();
				}

				// If the lock was not already released, add it to the runners future queue
				if (lock.promise.canBeSet()) {
					self->addRunner(lock, pPriority);
				}

				pml_debug_printf("    launched alreadyDone=%d priority=%d  %s\n",
				                 !lock.promise.canBeSet(),
				                 pPriority->priority,
				                 self->toString().c_str());
			}
		}
	}
};

#include "flow/unactorcompiler.h"

#endif
