/**
* CSC369 Assignment 2
*
* This is the source/implementation file for your safe stop sign
* submission code.
*/
#include "safeStopSign.h"


/**
 * Helper lock function
 */
void lock(pthread_mutex_t* mutex) {
	int rv = pthread_mutex_lock(mutex);
	if (rv != 0) {
		perror("Mutex lock failed."
				"@ " __FILE__ " : " LINE_STRING "\n");
	}
}

void initSafeStopSign(SafeStopSign* sign, int count) {
	initStopSign(&sign->base, count);

    // initialize the locks and car queues per lane
    for (int i = 0; i < QUADRANT_COUNT; i++) {
        initMutex(&sign->quadLocks[i]);
        initMutex(&sign->laneLocks[i]);
        sign->enterToken[i] = 0;
        sign->exitToken[i] = 0;
    }
    initMutex(&sign->masterQuadLock);
    initMutex(&sign->orderLock);
    initConditionVariable(&sign->order);
}

void destroySafeStopSign(SafeStopSign* sign) {
	destroyStopSign(&sign->base);

    for (int i = 0; i < QUADRANT_COUNT; i++) {
        pthread_mutex_destroy(&sign->quadLocks[i]);
        pthread_mutex_destroy(&sign->laneLocks[i]);
    }
    pthread_mutex_destroy(&sign->masterQuadLock);
    pthread_mutex_destroy(&sign->orderLock);
    pthread_cond_destroy(&sign->order);
}

void runStopSignCar(Car* car, SafeStopSign* sign) {

    // --------------------- Phase 0: Initializing ---------------------
    // give this car a unique token that is used for ordering
    int myToken;
    car->userPtr = &myToken;
    int myLane = car->position; // one of the four lanes (NORTH, SOUTH, ...)
    EntryLane* lane = getLane(car, &sign->base);

    // acquire lane lock before entering the lane
    lock(&sign->laneLocks[myLane]);
	enterLane(car, lane);

    // take an enter token number for a lane, then update
    lock(&sign->orderLock);
    myToken = sign->enterToken[myLane];
    sign->enterToken[myLane]++;
    unlock(&sign->orderLock);

    unlock(&sign->laneLocks[myLane]);


    // ------------------ Phase 1: Through stop sign ------------------
    // find out what quadrants the car will be travelling through
    int quadrants[QUADRANT_COUNT];
	int quadrantCount = getStopSignRequiredQuadrants(car, quadrants);

    // acquire quadrant locks before going through the stop sign
    lock(&sign->masterQuadLock);
    for (int i = 0; i < quadrantCount; i++) {
        lock(&sign->quadLocks[quadrants[i]]);
	}
    unlock(&sign->masterQuadLock);

    // safe to proceed through intersection
	goThroughStopSign(car, &sign->base);

    for (int i = 0; i < quadrantCount; i++) {
        unlock(&sign->quadLocks[quadrants[i]]);
	}


    // ---------------------- Phase 2: Exiting ----------------------
    // exit intersection and secure exiting order
    lock(&sign->orderLock);
    while (myToken != sign->exitToken[myLane]) {
        pthread_cond_wait(&sign->order, &sign->orderLock);
    }
    // we are safe to exit the intersection in the proper order
	exitIntersection(car, lane);

    // update the exit token number
    sign->exitToken[myLane]++;
    pthread_cond_broadcast(&sign->order);
    unlock(&sign->orderLock);
}
