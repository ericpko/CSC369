/**
* CSC369 Assignment 2
*
* This is the source/implementation file for your safe traffic light
* submission code.
*/
#include "safeTrafficLight.h"
#include "safeStopSign.h"
#include "common.h"

void initSafeTrafficLight(SafeTrafficLight* light, int horizontal, int vertical) {
	initTrafficLight(&light->base, horizontal, vertical);

    // initialize locks for each lane and their CVs
    for (int i = 0; i < DIRECTION_COUNT; i++) {
        for (int j = 0; j < NUM_LANES; j++) {
            initMutex(&light->laneLocks[i][j]);
            initMutex(&light->orderLocks[i][j]);
            light->enterTokens[i][j] = 0;
            light->exitTokens[i][j] = 0;
        }
    }
    initMutex(&light->lightStateLock);
    initMutex(&light->actionLock);
    initMutex(&light->leftLock);
    initMutex(&light->exitLock);

    // initialize condition variables
    initConditionVariable(&light->straight);
    initConditionVariable(&light->northSouth);
    initConditionVariable(&light->westEast);
    initConditionVariable(&light->greenLight);
    initConditionVariable(&light->ordering);

}

void destroySafeTrafficLight(SafeTrafficLight* light) {
	destroyTrafficLight(&light->base);

    // destroy all the locks and condition variables
    for (int i = 0; i < DIRECTION_COUNT; i++) {
        for (int j = 0; j < NUM_LANES; j++) {
            pthread_mutex_destroy(&light->laneLocks[i][j]);
            pthread_mutex_destroy(&light->orderLocks[i][j]);
        }
    }
    pthread_mutex_destroy(&light->lightStateLock);
    pthread_mutex_destroy(&light->actionLock);
    pthread_mutex_destroy(&light->leftLock);
    pthread_mutex_destroy(&light->exitLock);

    // destroy all condition variables
    pthread_cond_destroy(&light->straight);
    pthread_cond_destroy(&light->northSouth);
    pthread_cond_destroy(&light->westEast);
    pthread_cond_destroy(&light->greenLight);
    pthread_cond_destroy(&light->ordering);

}

void runTrafficLightCar(Car* car, SafeTrafficLight* light) {


    // --------------------- Phase 0: Initializing ---------------------
    // initialize some variables
    int myToken;                    // this car's spot in their line
    int myPosition = car->position; // one of the four lanes (NORTH, SOUTH, ...)
    int myAction = car->action;
    EntryLane* lane = getLaneLight(car, &light->base);

    /**
     * Note: [myPosition][myAction] together specifies one of the 12 lanes
     * that this car could be currently waiting in.
     */


    // --------------------- Phase 1: Entering lane ---------------------
    // lock down the lane this car is entering
    lock(&light->laneLocks[myPosition][myAction]);
	enterLane(car, lane);

    // collect a unique token number (for ordering the cars in this lane)
    lock(&light->orderLocks[myPosition][myAction]);
    myToken = light->enterTokens[myPosition][myAction];
    light->enterTokens[myPosition][myAction]++;
    unlock(&light->orderLocks[myPosition][myAction]);

    unlock(&light->laneLocks[myPosition][myAction]);


    // ----------------- Phase 2: Entering intersection -----------------

    lock(&light->lightStateLock);
    /**
     * We will only exit out of this do-while loop when recheck is zero.
     * After a thread wakes up, we need to recheck ALL the light's states
     * because it might have changed.
     */
    LightState lightState;
    int recheck;
    do {
        // find out the current light's state
        lightState = getLightState(&light->base);
        recheck = 0;

        // wait if the light is green for the opposite direction
        if (lightState == NORTH_SOUTH) {
            while ((lightState == NORTH_SOUTH) && (myPosition == WEST || myPosition == EAST)) {
                pthread_cond_wait(&light->westEast, &light->lightStateLock);
                lightState = getLightState(&light->base);
                recheck = 1;
            }

        // wait if the light is green for the opposite direction
        } else if (lightState == EAST_WEST) {
            while ((lightState == EAST_WEST) && (myPosition == NORTH || myPosition == SOUTH)) {
                pthread_cond_wait(&light->northSouth, &light->lightStateLock);
                lightState = getLightState(&light->base);
                recheck = 1;
            }

        // try to enter the traffic light, but wait if it's red
        } else if (lightState == RED) {
            while (lightState == RED) {
                pthread_cond_wait(&light->greenLight, &light->lightStateLock);
                lightState = getLightState(&light->base);
                recheck = 1;
            }
        }

    } while (recheck);


    /**
     * When we make it here, we know that
     * 1) The light is not red
     * 2) The light is green for this car's direction
     * 3) It's safe to enter the traffic light
     */

    // If this car is going straight, grab the action lock before left-turners.
    // Cars going straight get prority.
    if (myAction == STRAIGHT) {
        lock(&light->actionLock);
    }
    enterTrafficLight(car, &light->base);
    unlock(&light->lightStateLock);


    // ------------------ Phase 3: Through intersection ------------------

    // Case 1: Making the left turn
    if (myAction == LEFT_TURN) {

        // grab the action lock
        lock(&light->actionLock);

        // get the straight car count from the opposite direction of myPosition
        CarPosition opposite = getOppositePosition(myPosition);
        int straightCount = getStraightCount(&light->base, opposite);

        // wait if there are cars going straight in the opposite direction
        while (straightCount > 0) {
            pthread_cond_wait(&light->straight, &light->actionLock);
            straightCount = getStraightCount(&light->base, opposite);
        }

        // it's safe to make the left turn
        actTrafficLight(car, &light->base, NULL, NULL, NULL);


    // Case 2: Driving straight
    } else if (myAction == STRAIGHT) {
        // Wake up the left turners after going through
        actTrafficLight(car, &light->base, NULL, NULL, NULL);
        pthread_cond_broadcast(&light->straight);


    // Case 3: Turning right
    } else {
        // Just grab the lock because the light is green and we're turning right
        lock(&light->actionLock);
        actTrafficLight(car, &light->base, NULL, NULL, NULL);
        pthread_cond_broadcast(&light->straight);
    }
    unlock(&light->actionLock);


    /**
     * The car has now proceeded safely through the intersection.
     * Since actTrafficLight is the only possible function that can
     * change the light state, we need to check if it has changed.
     * If it has, we need to wake the threads waiting for that direction.
     */

    lock(&light->lightStateLock);

    // We can't forget about the folks waiting in phase 2!
    // keep checking until the light changes from red.
    // (It probably isn't red, so this likely doesn't loop, but here for safety)
    do {
        lightState = getLightState(&light->base);

        if (lightState == NORTH_SOUTH) {
            pthread_cond_broadcast(&light->northSouth);
            pthread_cond_broadcast(&light->greenLight);
        } else if (lightState == EAST_WEST) {
            pthread_cond_broadcast(&light->westEast);
            pthread_cond_broadcast(&light->greenLight);
        }

    } while (lightState == RED);

    unlock(&light->lightStateLock);


    // ------------------ Phase 4: Exiting intersection ------------------

    lock(&light->exitLock);

    // wait until it's my turn to exit in the proper order
    while (myToken != light->exitTokens[myPosition][myAction]) {
        pthread_cond_wait(&light->ordering, &light->exitLock);
    }

    // when we make it here, we know we're in the proper order, so exit
    exitIntersection(car, lane);

    // update the exit token number for this lane
    light->exitTokens[myPosition][myAction]++;
    pthread_cond_broadcast(&light->ordering);

    unlock(&light->exitLock);
}
















/**
 * Note to self: In phase 4, it is faster to use
 * lock(&light->orderLocks[myPosition][myAction]), but after many many many
 * trials of testing on the teach.cs machines, they only work with a single
 * exiting lock.
 * Using lock(&light->orderLocks[myPosition][myAction]) instead of
 * lock(&light->exitLock) worked on my mac and a windows machine with
 * ./carsim light 600 300
 */

// valgrind --tool=helgrind ./carsim light 1 10
