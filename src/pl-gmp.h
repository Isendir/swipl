/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        wielemak@science.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2005, University of Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef O_PLGMP_INCLUDED
#define O_PLGMP_INCLUDED

#define COMMON(type) SO_LOCAL type

#ifdef O_GMP
#include <gmp.h>

COMMON(void) 	initGMP(void);
COMMON(void)	get_integer(word w, number *n);
COMMON(void)	promoteToMPZNumber(number *n);
COMMON(void)	promoteToMPQNumber(number *n);
COMMON(void)	clearGMPNumber(Number n);
COMMON(void)	addMPZToBuffer(Buffer b, mpz_t mpz);
COMMON(char *)	loadMPZFromCharp(const char *data, Word r, Word *store);
COMMON(char *)	skipMPZOnCharp(const char *data);

#define clearNumber(n) \
	if ( (n)->type != V_INTEGER ) clearGMPNumber(n)
#else /*O_GMP*/

#define get_integer(w, n) \
	do \
	{ (n)->type = V_INTEGER; \
	  (n)->value.i = valInteger(w); \
	} while(0)

#define clearNumber(n)	(void)0
#define initGMP()	(void)0

#endif /*O_GMP*/

#define same_type_numbers(n1, n2) \
	if ( (n1)->type != (n2)->type ) \
	  make_same_type_numbers(n1, n2)


		 /*******************************
		 *	 COMMON FUNCTIONS	*
		 *******************************/

COMMON(int)	PL_unify_number(term_t t, Number n);
COMMON(void)	get_number(word w, Number n  ARG_LD);
COMMON(int)	PL_get_number(term_t t, Number n);
COMMON(word)	put_number__LD(Number n ARG_LD);
COMMON(void)	promoteToRealNumber(Number n);
COMMON(void)	make_same_type_numbers(Number n1, Number n2);
COMMON(void)    promoteNumber(Number n1, numtype type);
COMMON(int)	cmpNumbers(Number n1, Number n2);
COMMON(void)	cpNumber(Number to, Number from);

#endif /*O_PLGMP_INCLUDED*/
