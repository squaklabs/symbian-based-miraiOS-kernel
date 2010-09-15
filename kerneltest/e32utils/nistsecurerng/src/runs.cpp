/*
* Portions Copyright (c) 2009 Nokia Corporation and/or its subsidiary(-ies).
* All rights reserved.
* This component and the accompanying materials are made available
* under the terms of "Eclipse Public License v1.0"
* which accompanies this distribution, and is available
* at the URL "http://www.eclipse.org/legal/epl-v10.html".
*
* Initial Contributors:
* Nokia Corporation - initial contribution.
*
* Contributors:
*
* Description: 
* The original NIST Statistical Test Suite code is placed in public domain.
* (http://csrc.nist.gov/groups/ST/toolkit/rng/documentation_software.html) 
* 
* This software was developed at the National Institute of Standards and Technology by 
* employees of the Federal Government in the course of their official duties. Pursuant
* to title 17 Section 105 of the United States Code this software is not subject to 
* copyright protection and is in the public domain. The NIST Statistical Test Suite is
* an experimental system. NIST assumes no responsibility whatsoever for its use by other 
* parties, and makes no guarantees, expressed or implied, about its quality, reliability, 
* or any other characteristic. We would appreciate acknowledgment if the software is used.
*/

#include "openc.h"
#include "../include/externs.h"
#include "../include/cephes.h"

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
                              R U N S  T E S T 
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void
Runs(int n)
{
	int		S, k;
	double	pi, V, erfc_arg, p_value;

	S = 0;
	for ( k=0; k<n; k++ )
		if ( epsilon[k] )
			S++;
	pi = (double)S / (double)n;

	if ( fabs(pi - 0.5) > (2.0 / sqrt(n)) ) {
		fprintf(stats[TEST_RUNS], "\t\t\t\tRUNS TEST\n");
		fprintf(stats[TEST_RUNS], "\t\t------------------------------------------\n");
		fprintf(stats[TEST_RUNS], "\t\tPI ESTIMATOR CRITERIA NOT MET! PI = %f\n", pi);
		p_value = 0.0;
	}
	else {

		V = 1;
		for ( k=1; k<n; k++ )
			if ( epsilon[k] != epsilon[k-1] )
				V++;
	
		erfc_arg = fabs(V - 2.0 * n * pi * (1-pi)) / (2.0 * pi * (1-pi) * sqrt(2*n));
		p_value = erfc(erfc_arg);
		
		fprintf(stats[TEST_RUNS], "\t\t\t\tRUNS TEST\n");
		fprintf(stats[TEST_RUNS], "\t\t------------------------------------------\n");
		fprintf(stats[TEST_RUNS], "\t\tCOMPUTATIONAL INFORMATION:\n");
		fprintf(stats[TEST_RUNS], "\t\t------------------------------------------\n");
		fprintf(stats[TEST_RUNS], "\t\t(a) Pi                        = %f\n", pi);
		fprintf(stats[TEST_RUNS], "\t\t(b) V_n_obs (Total # of runs) = %d\n", (int)V);
		fprintf(stats[TEST_RUNS], "\t\t(c) V_n_obs - 2 n pi (1-pi)\n");
		fprintf(stats[TEST_RUNS], "\t\t    -----------------------   = %f\n", erfc_arg);
		fprintf(stats[TEST_RUNS], "\t\t      2 sqrt(2n) pi (1-pi)\n");
		fprintf(stats[TEST_RUNS], "\t\t------------------------------------------\n");
		if ( isNegative(p_value) || isGreaterThanOne(p_value) )
			fprintf(stats[TEST_RUNS], "WARNING:  P_VALUE IS OUT OF RANGE.\n");

		fprintf(stats[TEST_RUNS], "%s\t\tp_value = %f\n\n", p_value < ALPHA ? "FAILURE" : "SUCCESS", p_value);
	}

	fprintf(results[TEST_RUNS], "%f\n", p_value);
}
