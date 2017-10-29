/*
 * Copyright (c) 2013 Chun-Ying Huang
 *
 * This file is part of GamingAnywhere (GA).
 *
 * GA is free software; you can redistribute it and/or modify it
 * under the terms of the 3-clause BSD License as published by the
 * Free Software Foundation: http://directory.fsf.org/wiki/License:BSD_3Clause
 *
 * GA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the 3-clause BSD License along with GA;
 * if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <pthread.h>
#ifndef WIN32
#include <unistd.h>
#endif

#include "ga-common.h"
#include "ga-avcodec.h"

using namespace std;

AVCodec*
ga_avcodec_find_encoder(const char **names, enum AVCodecID cid) {
	AVCodec *codec = NULL;
	if(names != NULL) {
		while(*names != NULL) {
			if((codec = avcodec_find_encoder_by_name(*names)) != NULL)
				return codec;
			names++;
		}
	}
	if(cid != AV_CODEC_ID_NONE)
		return avcodec_find_encoder(cid);
	return NULL;
}

AVCodec*
ga_avcodec_find_decoder(const char **names, enum AVCodecID cid) {
	AVCodec *codec = NULL;
	if(names != NULL) {
		while(*names != NULL) {
			if((codec = avcodec_find_decoder_by_name(*names)) != NULL)
				return codec;
			names++;
		}
	}
	if(cid != AV_CODEC_ID_NONE)
		return avcodec_find_decoder(cid);
	return NULL;
}
