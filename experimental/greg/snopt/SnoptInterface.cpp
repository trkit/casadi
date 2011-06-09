// SnoptInterface.cpp
// Greg Horn
// Casadi 2011

#include <stdio.h>
#include <string.h>
#include <iostream>

#include <cstdlib>

#include "SnoptInterface.hpp"


static SnoptInterface * si;

using namespace std;
using namespace CasADi;

SnoptInterface::~SnoptInterface()
{
	delete []iGfun;
	delete []jGvar;

	delete []iAfun;
	delete []jAvar;
	delete []A;

	delete []x;
	delete []xlow;
	delete []xupp;
	delete []xmul;
	delete []xstate;

	delete []F;
	delete []Flow;
	delete []Fupp;
	delete []Fmul;
	delete []Fstate;
}

SnoptInterface::SnoptInterface(const SXFunction& user_F) : Ftotal(user_F)
{
	Ftotal.init();
	designVariables = &Ftotal.inputSX();

	si = this;

	init();
}

SnoptInterface::SnoptInterface(const OcpMultipleShooting& ocp)
{
	ftotal = vertcat( SXMatrix(ocp.objFun), ocp.g );

	Ftotal = SXFunction(ocp.designVariables, ftotal);
	Ftotal.init();

	designVariables = &ocp.designVariables;

	si = this;

	init();

	// overwrite initial guess
	copy( ocp.guess.begin(), ocp.guess.end(), x);

	// overwrite default xlow/xupp
	copy( ocp.lb.begin(), ocp.lb.end(), xlow);
	copy( ocp.ub.begin(), ocp.ub.end(), xupp);

    // overwrite Flow/Fupp
	copy( ocp.gMin.begin(), ocp.gMin.end(), &Flow[1]);
	copy( ocp.gMax.begin(), ocp.gMax.end(), &Fupp[1]);

	// for (int k=0; k < neA; k++)
	// 	cout << "A[" << iAfun[k] << "," << jAvar[k] << "]: " << A[k] << endl;
	// cout << endl;
	// for (int k=0; k < neG; k++)
	// 	cout << "G[" << iGfun[k] << "," << jGvar[k] << "]: " << Gfcn.outputSX().getElement(k) << endl;
}


void
SnoptInterface::init()
{
	/************ design variables ************/
	n = Ftotal.input().size();
	x = new doublereal[n];
	xlow = new doublereal[n];
	xupp = new doublereal[n];
	xmul = new doublereal[n];
	xstate = new integer[n];
	for (int k=0; k<n; k++){
		x[k] = 0;
		xlow[k] = -SNOPT_INFINITY;
		xupp[k] = SNOPT_INFINITY;
		xmul[k] = 0;
		xstate[k] = 0;
	}

	/*********** objective/constraint functions ***********/
	neF = Ftotal.output().size();
	objAdd = 0.0;
	objRow = FIRST_FORTRAN_INDEX;
	F = new doublereal[neF];
	Flow = new doublereal[neF];
	Fupp = new doublereal[neF];
	Fmul = new doublereal[neF];
	Fstate = new integer[neF];
	for (int k=0; k<neF; k++){
		F[k] = 0;
		Flow[k] = -SNOPT_INFINITY;
		Fupp[k] = 0;
		Fmul[k] = 0;
		Fstate[k] = 0;
	}
	Fupp[ objRow - FIRST_FORTRAN_INDEX ] = SNOPT_INFINITY;

	/****************** jacobian *********************/
	SXMatrix fnonlinear = ftotal;

	SXFunction gradF(Ftotal.jacobian());

	vector<int> rowind,col;
	gradF.output().sparsity().getSparsityCRS(rowind,col);

	// split jacobian into constant and nonconstant elements (linear and nonlinear parts of F)
	vector<doublereal> A_;
	vector<integer> iAfun_;
	vector<integer> jAvar_;

	vector<SX> G_;
	vector<integer> iGfun_;
	vector<integer> jGvar_;

	for(int r=0; r<rowind.size()-1; ++r)
        for(int el=rowind[r]; el<rowind[r+1]; ++el)
			if (gradF.outputSX().getElement(r, col[el]).isConstant()){
				A_.push_back( gradF.outputSX().getElement(r, col[el]).getValue() );
				iAfun_.push_back( r + FIRST_FORTRAN_INDEX );
				jAvar_.push_back( col[el] + FIRST_FORTRAN_INDEX );

				// subtract out linear part
				SXMatrix linearpart = gradF.outputSX().getElement(r, col[el])*(*designVariables)[col[el]];
				fnonlinear[r] -= linearpart[0];
				simplify(fnonlinear[r]);
			} else {
				G_.push_back( gradF.outputSX().getElement(r, col[el]) );
				iGfun_.push_back( r + FIRST_FORTRAN_INDEX );
				jGvar_.push_back( col[el] + FIRST_FORTRAN_INDEX );
			}
	
	// nonlinear function
	Fnonlinear = SXFunction( *designVariables, fnonlinear );
	Fnonlinear.init();

	// linear part
	neA = A_.size();
	lenA = neA;
	if (lenA == 0) lenA = 1;

	A = new doublereal[lenA];
	iAfun = new integer[lenA];
	jAvar = new integer[lenA];

	copy( A_.begin(), A_.end(), A);
	copy( iAfun_.begin(), iAfun_.end(), iAfun);
	copy( jAvar_.begin(), jAvar_.end(), jAvar);
	
	// nonlinear part
	neG = G_.size();
	lenG = neG;
	if (lenG == 0) lenG = 1;

	iGfun = new integer[lenG];
	jGvar = new integer[lenG];

	copy( iGfun_.begin(), iGfun_.end(), iGfun);
	copy( jGvar_.begin(), jGvar_.end(), jGvar);

	Gfcn = SXFunction( Ftotal.inputSX(), G_ );
	Gfcn.init();
}

void
SnoptInterface::run()
{
// #define LENRW 20000
// #define LENIW 10000
 #define LENCW 500

#define LENRW 600000
#define LENIW 150000
//#define LENCW 5000

	integer    minrw, miniw, mincw;
	integer    lenrw = LENRW, leniw = LENIW, lencw = LENCW;
	doublereal rw[LENRW];
	integer    iw[LENIW];
	char       cw[8*LENCW];

	integer    Cold = 0, Basis = 1, Warm = 2;

	integer    INFO;

	integer    nxname = 1, nFname = 1, npname;
	char       xnames[1*8], Fnames[1*8];
	char       Prob[200];

	integer    iSpecs = 4,  spec_len;
	integer    iSumm  = 6;
	integer    iPrint = 9,  prnt_len;

	char       printname[200];
	char       specname[200];

	integer    nS, nInf;
	doublereal sInf;
	integer    DerOpt, Major, strOpt_len;
	char       strOpt[200];

	/* open output files using snfilewrappers.[ch] */
	sprintf(specname ,   "%s", "sntoya.spc");   spec_len = strlen(specname);
	sprintf(printname,   "%s", "sntoya.out");   prnt_len = strlen(printname);

	/* Open the print file, fortran style */
	snopenappend_
		( &iPrint, printname,   &INFO, prnt_len );

	/*     ================================================================== */
	/*     First,  sninit_ MUST be called to initialize optional parameters   */
	/*     to their default values.                                           */
	/*     ================================================================== */

	sninit_
		( &iPrint, &iSumm, cw, &lencw, iw, &leniw, rw, &lenrw, 8*500 );

	strcpy(Prob,"snopta");
	npname = strlen(Prob);
	INFO = 0;

	/* Read in specs file (optional) */
	/* snfilewrapper_ will open the specs file, fortran style, */
	/* then call snspec_ to read in specs.                        */

	// snfilewrapper_
	// 	( specname, &iSpecs, &INFO, cw, &lencw,
	// 	  iw, &leniw, rw, &lenrw, spec_len, 8*lencw);

	// if( INFO != 101 )
    // {
	// 	printf("Warning: trouble reading specs file %s \n", specname);
    // }


	// sprintf(strOpt,"%s","Solution yes");
	// strOpt_len = strlen(strOpt);
	// snset_
	// 	( strOpt, &iPrint, &iSumm, &INFO,
	// 	  cw, &lencw, iw, &leniw, rw, &lenrw, strOpt_len, 8*500 );

	/*     ------------------------------------------------------------------ */
	/*     Tell SnoptA that userfg computes derivatives.                      */
	/*     ------------------------------------------------------------------ */

	DerOpt = 1;
	sprintf(strOpt,"%s","Derivative option");
	strOpt_len = strlen(strOpt);
	snseti_
		( strOpt, &DerOpt, &iPrint, &iSumm, &INFO,
		  cw, &lencw, iw, &leniw, rw, &lenrw, strOpt_len, 8*500 );

	//	Major = 250;
	Major = 2500;
	strcpy( strOpt,"Major Iterations limit");
	strOpt_len = strlen(strOpt);
	snseti_
		( strOpt, &Major, &iPrint, &iSumm, &INFO,
		  cw, &lencw, iw, &leniw, rw, &lenrw, strOpt_len, 8*500 );


	integer Minor = 1000;
	strcpy( strOpt,"Minor Iterations limit");
	strOpt_len = strlen(strOpt);
	snseti_
		( strOpt, &Minor, &iPrint, &iSumm, &INFO,
		  cw, &lencw, iw, &leniw, rw, &lenrw, strOpt_len, 8*500 );

	integer Niter = 100000;
	//integer Niter = 10000;
	strcpy( strOpt,"Iterations limit");
	strOpt_len = strlen(strOpt);
	snseti_
		( strOpt, &Niter, &iPrint, &iSumm, &INFO,
		  cw, &lencw, iw, &leniw, rw, &lenrw, strOpt_len, 8*500 );

	// integer verifyLevel = 3;
	// strcpy( strOpt,"Verify level");
	// strOpt_len = strlen(strOpt);
	// snseti_
	// 	( strOpt, &verifyLevel, &iPrint, &iSumm, &INFO,
	// 	  cw, &lencw, iw, &leniw, rw, &lenrw, strOpt_len, 8*500 );

	/*     ------------------------------------------------------------------ */
	/*     Solve the problem                                                  */
	/*     ------------------------------------------------------------------ */
	snopta_
		( &Cold, &neF, &n, &nxname, &nFname,
		  &objAdd, &objRow, Prob, (U_fp)userfcn,
		  iAfun, jAvar, &lenA, &neA, A,
		  iGfun, jGvar, &lenG, &neG,
		  xlow, xupp, xnames, Flow, Fupp, Fnames,
		  x, xstate, xmul, F, Fstate, Fmul,
		  &INFO, &mincw, &miniw, &minrw,
		  &nS, &nInf, &sInf,
		  cw, &lencw, iw, &leniw, rw, &lenrw,
		  cw, &lencw, iw, &leniw, rw, &lenrw,
		  npname, 8*nxname, 8*nFname,
		  8*500, 8*500);

// extern int snopta_
// ( integer *start, integer *nef, integer *n, integer *nxname, integer *nfname,
//   doublereal *objadd, integer *objrow, char *prob, U_fp usrfun,
//   integer *iafun, integer *javar, integer *lena, integer *nea, doublereal *a,
//   integer *igfun, integer *jgvar, integer *leng, integer *neg,
//   doublereal *xlow, doublereal *xupp, char *xnames, doublereal *flow, doublereal *fupp, char *fnames,
//   doublereal *x, integer *xstate, doublereal *xmul, doublereal *f, integer *fstate, doublereal *fmul,
//   integer *inform, integer *mincw, integer *miniw, integer *minrw,
//   integer *ns, integer *ninf, doublereal *sinf,
//   char *cu, integer *lencu, integer *iu, integer *leniu, doublereal *ru, integer *lenru,
//   char *cw, integer *lencw, integer *iw, integer *leniw, doublereal *rw, integer *lenrw,
//   ftnlen prob_len, ftnlen xnames_len, ftnlen fnames_len,
//   ftnlen cu_len, ftnlen cw_len );

	snclose_( &iPrint );
	snclose_( &iSpecs );
}


int SnoptInterface::userfcn
( integer    *Status, integer *n,    doublereal x[],
  integer    *needF,  integer *neF,  doublereal F[],
  integer    *needG,  integer *neG,  doublereal G[],
  char       *cu,     integer *lencu,
  integer    iu[],    integer *leniu,
  doublereal ru[],    integer *lenru )
{
	if( *needF > 0 ) {
		si->Fnonlinear.setInput(x);
		si->Fnonlinear.evaluate();
		si->Fnonlinear.getOutput(F);
	}

	if( *needG > 0 ){
		si->Gfcn.setInput(x);
		si->Gfcn.evaluate();
		si->Gfcn.getOutput(G);
	}
	return 0;
}
