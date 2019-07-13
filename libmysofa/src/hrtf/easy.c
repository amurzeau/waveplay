/*
 * easy.c
 *
 *  Created on: 18.01.2017
 *      Author: hoene
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include "mysofa_export.h"
#include "mysofa.h"

/**
 *
 */

static struct MYSOFA_EASY* mysofa_open_default(const char *filename, float samplerate, int *filterlength, int *err, bool applyNorm, float neighbor_angle_step, float neighbor_radius_step)
{
	struct MYSOFA_EASY *easy = malloc(sizeof(struct MYSOFA_EASY));
	if(!easy) {
		*err = MYSOFA_NO_MEMORY;
		return NULL;
	}

	easy->lookup = NULL;
	easy->neighborhood = NULL;

	easy->hrtf = mysofa_load(filename, err);
	if (!easy->hrtf) {
		mysofa_close(easy);
		return NULL;
	}

	*err = mysofa_check(easy->hrtf);
	if (*err != MYSOFA_OK) {
		mysofa_close(easy);
		return NULL;
	}

	*err = mysofa_resample(easy->hrtf, samplerate);
	if (*err != MYSOFA_OK) {
		mysofa_close(easy);
		return NULL;
	}

	if( applyNorm ) {
		mysofa_loudness(easy->hrtf);
	}

/* does not sound well:
   mysofa_minphase(easy->hrtf,0.01);
*/

	mysofa_tocartesian(easy->hrtf);

	easy->lookup = mysofa_lookup_init(easy->hrtf);
	if (easy->lookup == NULL) {
		*err = MYSOFA_INTERNAL_ERROR;
		mysofa_close(easy);
		return NULL;
	}

	easy->neighborhood = mysofa_neighborhood_init_withstepdefine(easy->hrtf,
						      easy->lookup,neighbor_angle_step,neighbor_radius_step);

	*filterlength = easy->hrtf->N;

	easy->fir = malloc(easy->hrtf->N * easy->hrtf->R * sizeof(float));
	assert(easy->fir);

	return easy;
}

MYSOFA_EXPORT struct MYSOFA_EASY* mysofa_open(const char *filename, float samplerate, int *filterlength, int *err)
{
	return mysofa_open_default(filename,samplerate,filterlength,err,true,MYSOFA_DEFAULT_NEIGH_STEP_ANGLE,MYSOFA_DEFAULT_NEIGH_STEP_RADIUS);
}

MYSOFA_EXPORT struct MYSOFA_EASY* mysofa_open_no_norm(const char *filename, float samplerate, int *filterlength, int *err)
{
	return mysofa_open_default(filename,samplerate,filterlength,err,false,MYSOFA_DEFAULT_NEIGH_STEP_ANGLE,MYSOFA_DEFAULT_NEIGH_STEP_RADIUS);
}

MYSOFA_EXPORT struct MYSOFA_EASY* mysofa_open_advanced(const char *filename, float samplerate, int *filterlength, int *err, bool norm, float neighbor_angle_step, float neighbor_radius_step)
{
	return mysofa_open_default(filename,samplerate,filterlength,err,norm,neighbor_angle_step,neighbor_radius_step);
}

MYSOFA_EXPORT struct MYSOFA_EASY* mysofa_open_cached(const char *filename, float samplerate, int *filterlength, int *err)
{
	struct MYSOFA_EASY* res = mysofa_cache_lookup(filename, samplerate);
	if(res) {
		*filterlength = res->hrtf->N;
		return res;
	}
	res = mysofa_open(filename,samplerate,filterlength,err);
	if(res) {
		res = mysofa_cache_store(res,filename,samplerate);
	}
	return res;
}

MYSOFA_EXPORT void mysofa_getfilter_short(struct MYSOFA_EASY* easy, float x, float y, float z,
					  short *IRleft, short *IRright,
					  int *delayLeft, int *delayRight)
{
	float c[3];
	float delays[2];
	float *fl;
	float *fr;
	int nearest;
	int *neighbors;
	int i;

	c[0] = x;
	c[1] = y;
	c[2] = z;
	nearest = mysofa_lookup(easy->lookup, c);
	assert(nearest>=0);
	neighbors = mysofa_neighborhood(easy->neighborhood, nearest);
    
	mysofa_interpolate(easy->hrtf, c,
			   nearest, neighbors,
			   easy->fir, delays);

	*delayLeft  = delays[0] * easy->hrtf->DataSamplingRate.values[0];
	*delayRight = delays[1] * easy->hrtf->DataSamplingRate.values[0];

	fl = easy->fir;
	fr = easy->fir + easy->hrtf->N;
	for(i=easy->hrtf->N;i>0;i--) {
		*IRleft++  = *fl++ * 32767.;
		*IRright++ = *fr++ * 32767.;
	}
}

MYSOFA_EXPORT void mysofa_getfilter_float(struct MYSOFA_EASY* easy, float x, float y, float z,
					  float *IRleft, float *IRright,
					  float *delayLeft, float *delayRight)
{
	float c[3];
	float delays[2];
	float *fl;
	float *fr;
	int nearest;
	int *neighbors;
	int i;

	c[0] = x;
	c[1] = y;
	c[2] = z;
	nearest = mysofa_lookup(easy->lookup, c);
	assert(nearest>=0);
	neighbors = mysofa_neighborhood(easy->neighborhood, nearest);

	float nearestPoint[3];
	float askedPoint[3] = {x, y, z};
	memcpy(nearestPoint, easy->hrtf->SourcePosition.values + nearest * easy->hrtf->C, sizeof(float) * easy->hrtf->C);
	mysofa_c2s(nearestPoint);
	mysofa_c2s(askedPoint);

	printf("Nearest position: %.3f %.3f %3f => %.3f %.3f %3f\n",
	askedPoint[0], askedPoint[1], askedPoint[2],
	       nearestPoint[0],
	        nearestPoint[1],
	        nearestPoint[2]);
    
	float *res = mysofa_interpolate(easy->hrtf, c,
			   nearest, neighbors,
			   easy->fir, delays);

	*delayLeft  = delays[0];
	*delayRight = delays[1];

	fl = res;
	fr = res + easy->hrtf->N;
	for(i=easy->hrtf->N;i>0;i--) {
		*IRleft++  = *fl++;
		*IRright++ = *fr++;
	}
}

MYSOFA_EXPORT void mysofa_close(struct MYSOFA_EASY* easy)
{
	if(easy) {
		if(easy->fir)
			free(easy->fir);
		if(easy->neighborhood)
			mysofa_neighborhood_free(easy->neighborhood);
		if(easy->lookup)
			mysofa_lookup_free(easy->lookup);
		if(easy->hrtf)
			mysofa_free(easy->hrtf);
		free(easy);
	}
}

MYSOFA_EXPORT void mysofa_close_cached(struct MYSOFA_EASY* easy)
{
	mysofa_cache_release(easy);
}
