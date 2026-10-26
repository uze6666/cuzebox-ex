/*
 *  Versioned input movies (CZINPUT v1/v2)
 *
 *  Copyright (C) 2022
      Matt Pandina (Artcfox)
 *  CUzeBox is Copyright (C) 2016
 *    Sandor Zsuga (Jubatian)
 *  Uzem (the base of CUzeBox) is copyright (C)
 *    David Etherton,
 *    Eric Anderton,
 *    Alec Bourque (Uze),
 *    Filipe Rinaldi,
 *    Sandor Zsuga (Jubatian),
 *    Matt Pandina (Artcfox)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/



#ifndef CAPTURE_H
#define CAPTURE_H

/* Bump when core semantics can change deterministic execution. Not an ABI. */
#include "cu_hw_features.h"
#define CAPTURE_BEHAVIOR_ID (0x20261015U ^ (CU_HW_FEATURES << 24))



#include "types.h"

/* CZINPUT v1/v2 movies. Reset choice applies only to recording; replay restores
 * the embedded starting snapshot, regardless of current bootloader settings. */
/* Preference for subsequent recordings; playback detects its own format. */
void capture_set_compact(boole compact);
boole capture_get_compact(void);
void capture_inputs(void);
boole capture_frame_end(void);
/* TRUE on end/error; no further frame should be executed. */
boole capture_replay(void);
boole capture_start(char const* name, boole replay);
boole capture_start_ex(char const* name, boole replay, boole reset_first);
boole capture_is_replaying(void);
boole capture_is_active(void);
char const* capture_get_status(void);

/*
** Closes the capture file, if opened
*/
void capture_finalize(void);
void capture_reset(void);
void capture_checkpoints_clear(void);
boole capture_checkpoint_take(unsigned slot);
boole capture_checkpoint_restore(unsigned slot);
boole capture_seek(uint64_t frame);
uint64_t capture_frame_number(void);
char const* capture_get_filename(void);


#endif
