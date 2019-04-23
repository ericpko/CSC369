#pragma once
/**
* CSC369 Assignment 2
*
* This is the header file for your safe traffic light submission code.
*/
#include "car.h"
#include "trafficLight.h"

#define NUM_LANES 3


typedef struct _OrderedCars {
	Car *car;
	struct _OrderedCars *nextCar;


} OrderedCars;


/**
* @brief Structure that you can modify as part of your solution to implement
* proper synchronization for the traffic light intersection.
*
* This is basically a wrapper around TrafficLight, since you are not allowed to
* modify or directly access members of TrafficLight.
*/
typedef struct _SafeTrafficLight {

	/**
	* @brief The underlying light.
	*
	* You are not allowed to modify the underlying traffic light or directly
	* access its members. All interactions must be done through the functions
	* you have been provided.
	*/
	TrafficLight base;

    // Locks
    pthread_mutex_t laneLocks[DIRECTION_COUNT][NUM_LANES];
    pthread_mutex_t orderLocks[DIRECTION_COUNT][NUM_LANES];
    pthread_mutex_t actionLock;
    pthread_mutex_t leftLock;
    pthread_mutex_t lightStateLock;
    pthread_mutex_t exitLock;

    // CV's
    pthread_cond_t ordering;
    pthread_cond_t straight;
    pthread_cond_t northSouth;
    pthread_cond_t westEast;
    pthread_cond_t greenLight;
    int enterTokens[DIRECTION_COUNT][NUM_LANES];
    int exitTokens[DIRECTION_COUNT][NUM_LANES];


} SafeTrafficLight;

/**
* @brief Initializes the safe traffic light.
*
* @param light pointer to the instance of SafeTrafficLight to be initialized.
* @param eastWest total number of cars moving east-west.
* @param northSouth total number of cars moving north-south.
*/
void initSafeTrafficLight(SafeTrafficLight* light, int eastWest, int northSouth);

/**
* @brief Destroys the safe traffic light.
*
* @param light pointer to the instance of SafeTrafficLight to be destroyed.
*/
void destroySafeTrafficLight(SafeTrafficLight* light);

/**
* @brief Runs a car-thread in a traffic light scenario.
*
* @param car pointer to the car.
* @param light pointer to the traffic light intersection.
*/
void runTrafficLightCar(Car* car, SafeTrafficLight* light);
