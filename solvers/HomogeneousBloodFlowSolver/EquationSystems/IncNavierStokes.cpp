///////////////////////////////////////////////////////////////////////////////
//
// File IncNavierStokes.cpp
//
// For more information, please see: http://www.nektar.info
//
// The MIT License
//
// Copyright (c) 2006 Division of Applied Mathematics, Brown University (USA),
// Department of Aeronautics, Imperial College London (UK), and Scientific
// Computing and Imaging Institute, University of Utah (USA).
//
// License for the specific language governing rights and limitations under
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// Description: Incompressible Navier Stokes class definition built on
// ADRBase class
//
////////////////////////////////////////////////////////////////////////////////

#include <HomogeneousBloodFlowSolver/EquationSystems/IncNavierStokes.h>
#include <LibUtilities/BasicUtils/Timer.h>
#include <cstdio>
#include <cstdlib>
#include <LibUtilities/Communication/Comm.h>
#include <SolverUtils/Filters/Filter.h>
#include <iomanip>
#include <math.h>

string IntToStr(int n) 
{
    stringstream result;
    result << n;
    return result.str();
}

namespace Nektar
{

    /**
     * Constructor. Creates ...
     *
     * \param 
     * \param
     */

    IncNavierStokes::IncNavierStokes(const LibUtilities::SessionReaderSharedPtr& pSession):
        //EquationSystem(pSession),
        UnsteadySystem(pSession),
        m_steadyStateSteps(0),
        m_subSteppingScheme(false),
        m_SmoothAdvection(false)
    {
    }

    void IncNavierStokes::v_InitObject()
    {

        int i,j;
        int numfields = m_fields.num_elements();
        std::string velids[] = {"u","v","w"};

        // Set up Velocity field to point to the first m_expdim of m_fields; 
        m_velocity = Array<OneD,int>(m_spacedim);

        for(i = 0; i < m_spacedim; ++i)
        {
            for(j = 0; j < numfields; ++j)
            {
                std::string var = m_boundaryConditions->GetVariable(j);
                if(NoCaseStringCompare(velids[i],var) == 0)
                {
                    m_velocity[i] = j;
                    break;
                }

                if(j == numfields)
                {
                    std::string error = "Failed to find field: " + var; 
                    ASSERTL0(false,error.c_str());
                }
            }
        }

        // Set up equation type enum using kEquationTypeStr
        for(i = 0; i < (int) eEquationTypeSize; ++i)
        {
            bool match;
            m_session->MatchSolverInfo("EQTYPE",kEquationTypeStr[i],match,false);
            if(match)
            {
                m_equationType = (EquationType)i; 
                break;
            }
        }
        ASSERTL0(i != eEquationTypeSize,"EQTYPE not found in SOLVERINFO section");

        // Equation specific Setups 
        switch(m_equationType)
        {
            case eSteadyStokes: 
            case eSteadyOseen: 
            case eSteadyNavierStokes:
            case eSteadyLinearisedNS: 
                break;
            case eUnsteadyNavierStokes:
            case eUnsteadyStokes:
            case eUnsteadyViscoElastic:
                {

                    m_session->LoadParameter("IO_InfoSteps", m_infosteps, 0);
                    m_session->LoadParameter("IO_EnergySteps", m_energysteps, 0);
                    m_session->LoadParameter("SteadyStateSteps", m_steadyStateSteps, 0);
                    m_session->LoadParameter("SteadyStateTol", m_steadyStateTol, 1e-6);


                    // set up mdl file 
                    std::string   mdlname = m_session->GetSessionName() + ".mdl";

                    if (m_energysteps && m_comm->GetRank() == 0)
                    {
                        m_mdlFile.open(mdlname.c_str());
                    }

                    // check to see if any user defined boundary condition is
                    // indeed implemented

                    for(int n = 0; n < m_fields[0]->GetBndConditions().num_elements(); ++n)
                    {

                        // Time Dependent Boundary Condition (if no user
                        // defined then this is empty)
                        if (m_fields[0]->GetBndConditions()[n]->GetUserDefined() != SpatialDomains::eNoUserDefined)
                        {
                            if (m_fields[0]->GetBndConditions()[n]->GetUserDefined() != SpatialDomains::eTimeDependent)
                            {                     	     
                                if(m_fields[0]->GetBndConditions()[n]->GetUserDefined() != SpatialDomains::eI)
                                {  	 	 
                                    ASSERTL0(false,"Unknown USERDEFINEDTYPE boundary condition");
                                }
                            }
                        }
                    }
                }
                break;
            case eNoEquationType:
            default:
                ASSERTL0(false,"Unknown or undefined equation type");
        }

        // Set up viscoelastic model

        if(m_equationType == eUnsteadyViscoElastic)
        {

            for(i = 0; i < (int) eEquationViscoElasticTypeSize; ++i)
            {
                bool match;
                m_session->MatchSolverInfo("VISELTYPE",kEquationViscoElasticTypeStr[i],match,false);
                if(match)
                {

                    m_viscoelasticType = (ViscoElasticType)i; 
                    break;
                }
            }
        }

        ASSERTL0(i != eEquationViscoElasticTypeSize,"VISELTYPE not found in SOLVERINFO section");


        m_session->LoadParameter("Kinvis", m_kinvis);
        m_session->LoadParameter("Reynolds", m_Re); //there is two different Reynolds defined for Oldroyd-B and HomogeneousBloodModel.
        m_session->LoadParameter("Rmu", m_Rmu);
        m_session->LoadParameter("Weissenberg", m_We);
        if(m_viscoelasticType == eHomogeneousBloodModel)
        {
            m_session->LoadParameter("WeissenbergInf",m_WeInf);
            m_session->LoadParameter("etaN",m_etaN);
            m_session->LoadParameter("etaInf",m_etaInf);
        }
            
        //read the notation text above to know how to choose these coefficients
        // // case # 1
        // 	m_ReC = 1.0 ;
        // 	m_ReM1 = 1.0 ;
        // 	m_ReM2 = m_Rmu/m_Re ;
        // 	m_ReM3 = 1.0/m_Re ;
        //       m_ReM4=(1-m_Rmu);
        // 	m_Rep=1.0; // coeficient of \grad p

        // case #2 

        m_ReC = 1.0 ;
        m_ReM1 = 1.0 ;
        m_ReM2 = m_Rmu/m_Re ;
        m_ReM3 = 1.0/m_Re ;
        m_ReM4=(1-m_Rmu);
        m_Rep=1.0/m_Re; // coeficient of \grad p



        // // case #3


        // m_ReC = m_Re ;
        // m_ReM1 = m_Re ;
        // m_ReM2 = m_Rmu ;
        // m_ReM3 = 1.0 ;
        //m_ReM4=(1-m_Rmu);
        // m_Rep=1.0; // coeficient of \grad p


        //case #4

        // m_ReC = 1.0 ;
        // m_ReM1 = 1.0 ;
        // m_ReM2 = 1/m_Re ;
        // m_ReM3 = 1.0/m_Re ;
        // m_ReM4=(1-m_Rmu)/m_Rmu;
        // m_Rep=1.0/m_Re; // coeficient of \grad p

        //case #5

        // m_ReC = 1.0 ;
        // m_ReM1 = 1.0 ;
        // m_ReM2 = etaS/m_Re ;
        // m_ReM3 = 1.0/m_Re ;
        // m_ReM4 = Weissenberg;
        // m_Rep = 1.0/m_Re; // coeficient of \grad p



        if (m_equationType == eUnsteadyNavierStokes || m_equationType == eSteadyNavierStokes || m_equationType == eUnsteadyViscoElastic)
        {
            std::string vConvectiveType = "Convective";
            if (m_session->DefinesTag("AdvectiveType"))
            {
                vConvectiveType = m_session->GetTag("AdvectiveType");
            }
            m_advObject = GetAdvectionTermFactory().CreateInstance(vConvectiveType, m_session, m_graph);
        }

        if (m_equationType == eUnsteadyLinearisedNS)// || m_equationType == eSteadyNavierStokes)
        {
            std::string vConvectiveType = "Linearised";
            if (m_session->DefinesTag("AdvectiveType"))
            {
                //vConvectiveType = m_session->GetTag("Linearised");
                vConvectiveType = m_session->GetTag("AdvectiveType");
            }
            m_advObject = GetAdvectionTermFactory().CreateInstance(vConvectiveType, m_session, m_graph);
        }

        if(m_equationType == eUnsteadyStokes)
        {
            std::string vConvectiveType = "NoAdvection";
            m_advObject = GetAdvectionTermFactory().CreateInstance(vConvectiveType, m_session, m_graph);
        }

#if 0 // Not required if building on an UnsteadySystem rather than an EquationSystem
        // Set up filters
        LibUtilities::FilterMap::const_iterator x;
        LibUtilities::FilterMap f = m_session->GetFilters();
        for (x = f.begin(); x != f.end(); ++x)
        {
            m_filters.push_back(SolverUtils::GetFilterFactory().CreateInstance(x->first, m_session, x->second));
        }
#endif
    }

    IncNavierStokes::~IncNavierStokes(void)
    {
    }

    void IncNavierStokes::AdvanceInTime(int nsteps)
    {

        //      ofstream fp1("aggregatesize.dat",ios::out | ios:: app);
        //       ofstream fp2("Shearrate.dat",ios::out | ios:: app);
        //     ofstream fp5("noise_Comparison_L2Vx_We.dat",ios::out | ios:: app);
        //     ofstream fp6("noise_Comparison_L2Txx_We.dat",ios::out | ios:: app);
        ofstream fp7("Trackpoint_periodic_pressure.dat",ios::out | ios:: app);
        ofstream fp8("Trackpoint_periodic_w.dat",ios::out | ios:: app);
        int i,n;
        int phystot  = m_fields[0]->GetTotPoints();
        int n_fields = m_fields.num_elements();
        static int nchk = 0;
        m_time = m_timestep;

        Timer timer;

        if(m_HomogeneousType == eHomogeneous1D)
        {
            for(i = 0; i < n_fields; ++i)
            {
                m_fields[i]->HomogeneousFwdTrans(m_fields[i]->GetPhys(),m_fields[i]->UpdatePhys());
                m_fields[i]->SetWaveSpace(true);
                m_fields[i]->SetPhysState(false);
            }
        }

        // Set up wrapper to fields data storage. 
        Array<OneD, Array<OneD, NekDouble> >  fields(m_nConvectiveFields);
        for(i = 0; i < m_nConvectiveFields; ++i)
        {
            fields[i]  = m_fields[i]->UpdatePhys();
        }

        m_integrationSoln = m_integrationScheme[m_intSteps-1]->InitializeScheme(m_timestep, fields, m_time, m_integrationOps);


        Array<OneD, Array<OneD, NekDouble> >   fields_viscoelasticstress(m_nViscoElasticStressFields);
        //--------------------------------Me.--------------------------------------
    
        Array<OneD, Array<OneD, NekDouble> >   fields_homogeneousbloodmodel(1);
        cout  << "111\n";
       //cout << "here is my value: " << m_WeNew[10] <<endl;
        //cout << "here is my value: " << m_We <<endl;
        cout  << "222\n";
        //--------------------------------Me.--------------------------------------

        if(m_equationType == eUnsteadyViscoElastic)
        {

            //--------------------------------Me.--------------------------------------
           if(m_viscoelasticType == eHomogeneousBloodModel)
          
            {
                printf(" Im here111\n");
                fields_homogeneousbloodmodel[0]  = m_fields[m_nViscoElasticStressFields+m_spacedim]->UpdatePhys();
                printf(" Im here222\n");
                m_integrationSoln_aggregatesize = m_integrationScheme[m_intSteps-1]->InitializeScheme(m_timestep, fields_homogeneousbloodmodel, m_time,m_integrationOps_aggregatesize);
                printf("I'm here intialise\n");
                //exit(-1);
            }
            printf("test1");
            //--------------------------------Me.--------------------------------------
            //
            //
            //if(m_viscoelasticType == eHomogeneousBloodModel)
            //{ 
            //
            //}else{      
            
            for(i = 0; i < m_nViscoElasticStressFields; ++i)
            {
                fields_viscoelasticstress[i]  = m_fields[i+m_spacedim]->UpdatePhys();
            }
            printf("test2");
            //} //end of else before for
            
            m_integrationSoln_viscoelasticstress = m_integrationScheme[m_intSteps-1]->InitializeScheme(m_timestep, fields_viscoelasticstress, m_time,m_integrationOps_viscoelasticstress);

            printf("test3");
        }

        // Initialise NS solver which is set up to use a GLM method
        // with calls to EvaluateAdvection_SetPressureBCs and
        // SolveUnsteadyStokesSystem

        std::vector<SolverUtils::FilterSharedPtr>::iterator x;

        for (x = m_filters.begin(); x != m_filters.end(); ++x)
        {
            (*x)->Initialise(m_fields, m_time);
        }

        printf("test");
        //Time advance
        for(n = 0; n < nsteps; ++n)
        {
            m_numstep=n;

            timer.Start();

            if(m_subSteppingScheme)
            {
                SubStepSaveFields(n);
                SubStepAdvance(n);
            }

            // Advance viscoelastic fields
            if(m_equationType == eUnsteadyViscoElastic)
            {
            //--------------------------------Me.--------------------------------------
                if(m_viscoelasticType == eHomogeneousBloodModel)
                {
                fields_homogeneousbloodmodel = m_integrationScheme[min(n,m_intSteps-1)]->TimeIntegrate(m_timestep, m_integrationSoln_aggregatesize, m_integrationOps_aggregatesize);
                }
            //--------------------------------Me.--------------------------------------
                
                fields_viscoelasticstress = m_integrationScheme[min(n,m_intSteps-1)]->TimeIntegrate(m_timestep, m_integrationSoln_viscoelasticstress, m_integrationOps_viscoelasticstress);

                for(i = 0; i <m_nViscoElasticStressFields; ++i)
                {

                    m_fields[i+m_spacedim]->SetPhys(fields_viscoelasticstress[i]);
                    m_fields[i+m_spacedim]->FwdTrans_IterPerExp(fields_viscoelasticstress[i],m_fields[i+m_spacedim]->UpdateCoeffs());
                    m_fields[i+m_spacedim]->SetPhysState(true);

                }
            }


            // if(m_time<=20)
            //   {
            // fp1<<m_time<< " " << m_fields[m_spacedim+m_nViscoElasticStressFields]->GetPhys()[10]<< endl;
            // fp2<<m_time<< " " << 10*m_fields[3]->GetPhys()[10]<< endl;
            //   }
            // else
            //   {
            // fp1<<40-m_time<< " " << m_fields[m_spacedim+m_nViscoElasticStressFields]->GetPhys()[10]<< endl;
            // fp2<<40-m_time<< " " << 10*m_fields[3]->GetPhys()[10]<< endl;	
            //   }

            // Advance velocity fields
            fields = m_integrationScheme[min(n,m_intSteps-1)]->TimeIntegrate(m_timestep, m_integrationSoln, m_integrationOps);

            m_time += m_timestep;

            timer.Stop();



            fp7<<m_time<< " " << m_fields[m_spacedim+m_nViscoElasticStressFields]->GetPhys()[100]<< endl;

            // Write out energy data to file
            if(m_energysteps && !((n+1)%m_energysteps))
            {
                WriteModalEnergy();
            }


            // dump data in m_fields->m_coeffs to file. 
            if(m_checksteps && n&&(!((n+1)%m_checksteps)))
            {
                //fp5<<n<< " "<< L2Error(0,true) << endl;
                //fp6<<n<< " "<< L2Error(2,true) << endl;
                //WriteCheckpoint_Ouptput();
                if(m_HomogeneousType == eHomogeneous1D)
                {
                    for(i = 0; i< n_fields; i++)
                    {
                        m_fields[i]->SetWaveSpace(false);
                        m_fields[i]->BwdTrans(m_fields[i]->GetCoeffs(),m_fields[i]->UpdatePhys());
                        m_fields[i]->SetPhysState(true);
                    }
                    nchk++;
                    Checkpoint_Output(nchk);
                    for(i = 0; i< n_fields; i++)
                    {
                        m_fields[i]->SetWaveSpace(true);
                        m_fields[i]->HomogeneousFwdTrans(m_fields[i]->GetPhys(),m_fields[i]->UpdatePhys());
                        m_fields[i]->SetPhysState(false);
                    }
                }
                else
                {
                    for(i = 0; i < m_nConvectiveFields; ++i)
                    {
                        m_fields[i]->SetPhys(fields[i]);
                        m_fields[i]->SetPhysState(true);
                    }
                    nchk++;
                    Checkpoint_Output(nchk);
                }
            }


            if(m_steadyStateSteps && n && (!((n+1)%m_steadyStateSteps)))
            {



                if(CalcSteadyState() == true)
                {
                    cout << "Reached Steady State to tolerance " << m_steadyStateTol << endl; 
                    break;
                }
            }

            // Transform data into coefficient space
            if (m_filters.size() > 0)
            {
                for (i = 0; i < m_nConvectiveFields; ++i)
                {
                    m_fields[i]->FwdTrans_IterPerExp(fields[i],
                            m_fields[i]->UpdateCoeffs());
                    m_fields[i]->SetPhysState(false);
                }
            }


            std::vector<SolverUtils::FilterSharedPtr>::iterator x;
            for (x = m_filters.begin(); x != m_filters.end(); ++x)
            {
                (*x)->Update(m_fields, m_time);
            }

            fp8<<m_time<< " " << m_fields[m_spacedim-1]->GetPhys()[100]<< endl;

            //****************** write data file**********************
            /*

               if(n%m_checksteps==0)
               {		
               NekDouble R13=   -0.0232;
               NekDouble  R23=    -0.9995;
               NekDouble  R33=   -0.0235 ;	       

               ofstream outFile;
               string filename;



               Array<OneD,NekDouble> x1(m_fields[0]->GetExp(0)->GetTotPoints(), 0.0);
               Array<OneD,NekDouble> y1(m_fields[0]->GetExp(0)->GetTotPoints(), 0.0);
               Array<OneD,NekDouble> z1(m_fields[0]->GetExp(0)->GetTotPoints(), 0.0);


               filename="96moderateNewtonianNeumannBBC_" + IntToStr(n) +".dat";
               cout<< filename << "  \n"; 
               outFile.open(filename.c_str());


               outFile<< "Variables = x, y, z, w,p\n";


               for(int i=0;i<m_fields[0]->GetExpSize();i++)

               {
               m_fields[0]->GetExp(i)->GetCoords(x1,y1,z1);
            //m_fields[0]->GetExp(i)->SetPhys(m_fields[0]->GetPhys());
            //m_fields[1]->GetExp(i)->SetPhys(m_fields[1]->GetPhys());
            m_fields[0]->GetExp(i)->SetPhys(m_fields[0]->GetPhys());
            m_pressure->GetExp(i)->SetPhys(m_pressure->GetPhys());

            outFile << "Zone, I=" << m_fields[0]->GetExp(i)->GetNumPoints(0) << ", J=" << m_fields[0]->GetExp(i)->GetNumPoints(1) << ", K="<< m_fields[0]->GetExp(i)->GetNumPoints(2) << ", F=Point" << endl;

            //outFile << "Zone, I=" << m_fields[0]->GetExp(i)->GetNumPoints(0) << ", J=" << m_fields[0]->GetExp(i)->GetNumPoints(1) <<  ", F=Point" << endl;

            for(int j=0;j<m_fields[0]->GetExp(i)->GetTotPoints();j++)
            {

            outFile<< x1[j]<< " " << y1[j]<< " "<<z1[j]<< " " << m_fields[0]->GetExp(i)->GetPhys()[j]<< " "<< m_pressure->GetPhys()[j+m_pressure->GetPhys_Offset(i)]<<endl;



            }

            }


            }

*/	   

            //*******************************************************


        }


        if(m_HomogeneousType == eHomogeneous1D)
        {
            for(i = 0 ; i< n_fields ; i++)
            {
                m_fields[i]->SetWaveSpace(false);
                m_fields[i]->BwdTrans(m_fields[i]->GetCoeffs(),m_fields[i]->UpdatePhys());
                m_fields[i]->SetPhysState(true);
            }
        }
        else 
        {

            for(i = 0; i < m_nConvectiveFields; ++i)
            {
                m_fields[i]->SetPhys(fields[i]);
                m_fields[i]->SetPhysState(true);
            }
        }


        if (m_energysteps)
        {
            m_mdlFile.close();
        }

        for (x = m_filters.begin(); x != m_filters.end(); ++x)
        {
            (*x)->Finalise(m_fields, m_time);
        }
        //        int phystot = m_fields[0]->GetTotPoints();
        /*
           ofstream fp1("Ux.dat",ios::out | ios:: app);
           ofstream fp2("Txx.dat",ios::out | ios:: app);
           ofstream fp3("Txy.dat",ios::out | ios:: app);
           ofstream fp4("pressure.dat",ios::out | ios:: app);

           Array<OneD,NekDouble> x0(phystot,0.0);
           Array<OneD,NekDouble> y0(phystot,0.0);
           Array<OneD,NekDouble> z0(phystot,0.0);
           NekDouble y_an,dy=1.0/200.0;
           y_an=0.0;

           m_fields[0]->GetCoords(x0,y0,z0);
           for(i=0;i<200;i++)
           {
           y_an=y_an+dy;
           cout<<y_an<<endl;
        //fp1 <<y_an<<" "<<y_an*(1-y_an)<<endl;
        //fp2 <<y_an<<" "<<2*m_We*(1-m_Rmu)*(1-2*y_an)*(1-2*y_an)<<endl;
        //fp3 <<y_an<<" "<<(1-m_Rmu)*(1-2*y_an)<<endl;
        //fp4 <<y_an<<" "<<-2*m_kinvis*(0.5-1.0)<<endl;

        }


        for(i=0;i<phystot;++i)
        {
        if(z0[i]==0&& x0[i]==0.5)
        {
        // fp1 <<y0[i]<<" "<<m_fields[0]->GetPhys()[i]<<endl;
        //fp2 <<y0[i]<<" "<<m_fields[3]->GetPhys()[i]<<endl;
        //fp3 <<y0[i]<<" "<<m_fields[4]->GetPhys()[i]<<endl;
        fp4 <<y0[i]<<" "<<m_pressure->GetPhys()[i]<<endl;
        cout<<"y0="<< y0[i]<< endl;
        }
        }
        // fp1<<"zone"<< endl;
        // fp2<<"zone"<<endl;
        // fp3<<"zone"<<endl;
        fp4<<"zone"<<endl;
        */

        //	fp5<<m_We<< " "<< L2Error(0,true) << endl;
        //	fp6<<m_We<< " "<< L2Error(2,true) << endl;

        //fp5<<"zone"<<endl;
        //fp6<<"zone"<<endl;


        Array<OneD,NekDouble> x0(phystot,0.0);
        Array<OneD,NekDouble> y0(phystot,0.0);
        Array<OneD,NekDouble> z0(phystot,0.0);

        m_fields[0]->GetCoords(x0,y0,z0);
        //      cout<< "coordinate" << endl;


        //       cout<< "At the end" << endl;
        //      for(i=0; i<phystot;i++)
        // {
        //   cout<<"i="<< i<< " " <<m_fields[0]->GetPhys()[i]<<endl;
        // }

    }

    void IncNavierStokes::SubStepSaveFields(const int nstep)
    {
        int i,n;
        int nvel = m_velocity.num_elements();
        int npts = m_fields[0]->GetTotPoints();

        // rotate fields 
        int nblocks = m_previousVelFields.num_elements()/nvel; 
        Array<OneD, NekDouble> save; 

        for(n = 0; n < nvel; ++n)
        {
            save = m_previousVelFields[(nblocks-1)*nvel+n];

            for(i = nblocks-1; i > 0; --i)
            {
                m_previousVelFields[i*nvel+n] = m_previousVelFields[(i-1)*nvel+n];
            }

            m_previousVelFields[n] = save; 
        }

        // Put previous field 
        for(i = 0; i < nvel; ++i)
        {
            m_fields[m_velocity[i]]->BwdTrans(m_fields[m_velocity[i]]->GetCoeffs(),
                    m_fields[m_velocity[i]]->UpdatePhys());
            Vmath::Vcopy(npts,m_fields[m_velocity[i]]->GetPhys(),1,
                    m_previousVelFields[i],1);   
        }

        if(nstep == 0)// initialise all levels with first field 
        {
            for(n = 0; n < nvel; ++n)
            {
                for(i = 1; i < nblocks; ++i)
                {
                    Vmath::Vcopy(npts,m_fields[m_velocity[n]]->GetPhys(),1,
                            m_previousVelFields[i*nvel+n],1);   

                }
            }
        }



    }

    void IncNavierStokes::SubStepAdvance(const int nstep)
    {
        int n;
        int nsubsteps, minsubsteps;

        NekDouble dt; 
        NekDouble time = m_time;

        Array<OneD, Array<OneD, NekDouble> > fields, velfields;

        static int ncalls = 1;
        int  nint         = min(ncalls++, m_intSteps);

        Array<OneD, NekDouble> CFL(m_fields[0]->GetExpSize(), 
                m_cflSafetyFactor);

        // Get the proper time step with CFL control
        dt = GetSubstepTimeStep();

        nsubsteps = (m_timestep > dt)? ((int)(m_timestep/dt)+1):1; 
        m_session->LoadParameter("MinSubSteps", minsubsteps,0);
        nsubsteps = max(minsubsteps, nsubsteps);

        dt = m_timestep/nsubsteps;

        if (m_infosteps && !((nstep+1)%m_infosteps) && m_comm->GetRank() == 0)
        {
            cout << "Sub-integrating using "<< nsubsteps 
                << " steps over Dt = "     << m_timestep 
                << " (SubStep CFL="        << m_cflSafetyFactor << ")"<< endl;
        }

        for (int m = 0; m < nint; ++m)
        {
            // We need to update the fields held by the m_integrationSoln
            fields = m_integrationSoln->UpdateSolutionVector()[m];

            // Initialise NS solver which is set up to use a GLM method
            // with calls to EvaluateAdvection_SetPressureBCs and
            // SolveUnsteadyStokesSystem
            LibUtilities::TimeIntegrationSolutionSharedPtr 
                SubIntegrationSoln = m_subStepIntegrationScheme->
                InitializeScheme(dt, fields, time, m_subStepIntegrationOps);

            for(n = 0; n < nsubsteps; ++n)
            {
                fields = m_subStepIntegrationScheme->
                    TimeIntegrate(
                            dt, SubIntegrationSoln, m_subStepIntegrationOps);
            }

            // Reset time integrated solution in m_integrationSoln 
            m_integrationSoln->SetSolVector(m,fields);
        }
    }

    /** 
     * Explicit Advection terms used by SubStepAdvance time integration
     */
    void IncNavierStokes::SubStepAdvection(
            const Array<OneD, const Array<OneD, NekDouble> > &inarray,  
            Array<OneD, Array<OneD,       NekDouble> > &outarray, 
            const NekDouble time)
    {

        int i;
        int nVariables     = inarray.num_elements();
        int nQuadraturePts = inarray[0].num_elements();

        /// Get the number of coefficients
        int ncoeffs = m_fields[0]->GetNcoeffs(); 

        /// Define an auxiliary variable to compute the RHS 
        Array<OneD, Array<OneD, NekDouble> > WeakAdv(nVariables);
        WeakAdv[0] = Array<OneD, NekDouble> (ncoeffs*nVariables);
        for(i = 1; i < nVariables; ++i)
        {
            WeakAdv[i] = WeakAdv[i-1] + ncoeffs;
        }

        //cout << "Time: " << time << endl;
        Array<OneD, Array<OneD, NekDouble> > Velfields(m_velocity.num_elements());
        Array<OneD, int> VelIds(m_velocity.num_elements());

        Velfields[0] = Array<OneD, NekDouble> (nQuadraturePts*m_velocity.num_elements());

        for(i = 1; i < m_velocity.num_elements(); ++i)
        {
            Velfields[i] = Velfields[i-1] + nQuadraturePts; 
        }
        SubStepExtrapoloteField(fmod(time,m_timestep), Velfields);

        m_advObject->DoAdvection(m_fields, Velfields, inarray, outarray, time);

        for(i = 0; i < nVariables; ++i)
        {
            m_fields[i]->IProductWRTBase(outarray[i],WeakAdv[i]); 
            // negation requried due to sign of DoAdvection term to be consistent
            Vmath::Neg(ncoeffs, WeakAdv[i], 1);
        }

        AddAdvectionPenaltyFlux(Velfields, inarray, WeakAdv);

        /// Operations to compute the RHS
        for(i = 0; i < nVariables; ++i)
        {
            // Negate the RHS
            Vmath::Neg(ncoeffs, WeakAdv[i], 1);

            /// Multiply the flux by the inverse of the mass matrix
            m_fields[i]->MultiplyByElmtInvMass(WeakAdv[i], WeakAdv[i]);

            /// Store in outarray the physical values of the RHS
            m_fields[i]->BwdTrans(WeakAdv[i], outarray[i]);
        }

        //add the force
        if(m_session->DefinesFunction("BodyForce"))
        {
            if(m_SingleMode || m_HalfMode)
            {
                for(int i = 0; i < m_nConvectiveFields; ++i)
                {
                    m_forces[i]->SetWaveSpace(true);	
                    m_forces[i]->BwdTrans(m_forces[i]->GetCoeffs(),
                            m_forces[i]->UpdatePhys());
                }
            }

            int nqtot = m_fields[0]->GetTotPoints();
            for(int i = 0; i < m_nConvectiveFields; ++i)
            {
                Vmath::Vadd(nqtot,outarray[i],1,(m_forces[i]->GetPhys()),1,outarray[i],1);
            }
        }
    }

    void IncNavierStokes::v_GetFluxVector(const int i, 
            Array<OneD, Array<OneD, NekDouble> > &physfield,
            Array<OneD, Array<OneD, NekDouble> > &flux)
    {
        ASSERTL1(flux.num_elements() == m_velocity.num_elements(),"Dimension of flux array and velocity array do not match");

        for(int j = 0; j < flux.num_elements(); ++j)
        {
            Vmath::Vmul(GetNpoints(), physfield[i], 1, m_fields[m_velocity[j]]->GetPhys(), 1, flux[j], 1);
        }
    }

    void IncNavierStokes::v_NumericalFlux(Array<OneD, Array<OneD, NekDouble> > &physfield, 
            Array<OneD, Array<OneD, NekDouble> > &numflux)
    {
        /// Counter variable
        int i;

        /// Number of trace points
        int nTracePts   = GetTraceNpoints();

        /// Number of spatial dimensions
        int nDimensions = m_spacedim;

        /// Forward state array
        Array<OneD, NekDouble> Fwd(2*nTracePts);

        /// Backward state array
        Array<OneD, NekDouble> Bwd = Fwd + nTracePts;

        /// Normal velocity array
        Array<OneD, NekDouble> Vn (nTracePts, 0.0);

        // Extract velocity field along the trace space and multiply by trace normals
        for(i = 0; i < nDimensions; ++i)
        {
            m_fields[0]->ExtractTracePhys(m_fields[m_velocity[i]]->GetPhys(), Fwd);
            Vmath::Vvtvp(nTracePts, m_traceNormals[i], 1, Fwd, 1, Vn, 1, Vn, 1);
        }

        /// Compute the numerical fluxes at the trace points
        for(i = 0; i < numflux.num_elements(); ++i)
        {
            /// Extract forwards/backwards trace spaces
            m_fields[i]->GetFwdBwdTracePhys(physfield[i], Fwd, Bwd);

            /// Upwind between elements
            m_fields[i]->GetTrace()->Upwind(Vn, Fwd, Bwd, numflux[i]);

            /// Calculate the numerical fluxes multipling Fwd or Bwd
            /// by the normal advection velocity
            Vmath::Vmul(nTracePts, numflux[i], 1, Vn, 1, numflux[i], 1);
        }
    }

    void IncNavierStokes::AddAdvectionPenaltyFlux(const Array<OneD, const Array<OneD, NekDouble> > &velfield, 
            const Array<OneD, const Array<OneD, NekDouble> > &physfield, 
            Array<OneD, Array<OneD, NekDouble> > &Outarray)
    {
        ASSERTL1(physfield.num_elements() == Outarray.num_elements(),"Physfield and outarray are of different dimensions");

        int i;

        /// Number of trace points
        int nTracePts   = GetTraceNpoints();

        /// Number of spatial dimensions
        int nDimensions = m_spacedim;

        /// Forward state array
        Array<OneD, NekDouble> Fwd(3*nTracePts);

        /// Backward state array
        Array<OneD, NekDouble> Bwd = Fwd + nTracePts;

        /// upwind numerical flux state array
        Array<OneD, NekDouble> numflux = Bwd + nTracePts;

        /// Normal velocity array
        Array<OneD, NekDouble> Vn (nTracePts, 0.0);

        // Extract velocity field along the trace space and multiply by trace normals
        for(i = 0; i < nDimensions; ++i)
        {
            m_fields[0]->ExtractTracePhys(m_fields[m_velocity[i]]->GetPhys(), Fwd);
            Vmath::Vvtvp(nTracePts, m_traceNormals[i], 1, Fwd, 1, Vn, 1, Vn, 1);
        }

        for(i = 0; i < physfield.num_elements(); ++i)
        {
            /// Extract forwards/backwards trace spaces
            /// Note: Needs to have correct i value to get boundary conditions
            m_fields[i]->GetFwdBwdTracePhys(physfield[i], Fwd, Bwd);

            /// Upwind between elements
            m_fields[0]->GetTrace()->Upwind(Vn, Fwd, Bwd, numflux);

            /// Construct difference between numflux and Fwd,Bwd
            Vmath::Vsub(nTracePts, numflux, 1, Fwd, 1, Fwd, 1);
            Vmath::Vsub(nTracePts, numflux, 1, Bwd, 1, Bwd, 1);

            /// Calculate the numerical fluxes multipling Fwd, Bwd and
            /// numflux by the normal advection velocity
            Vmath::Vmul(nTracePts, Fwd, 1, Vn, 1, Fwd, 1);
            Vmath::Vmul(nTracePts, Bwd, 1, Vn, 1, Bwd, 1);

            m_fields[0]->AddFwdBwdTraceIntegral(Fwd,Bwd,Outarray[i]);
        }
    }

    /** 
     * Projection used by SubStepAdvance time integration
     */
    void IncNavierStokes::SubStepProjection(const Array<OneD, const Array<OneD, NekDouble> > &inarray,  Array<OneD, Array<OneD, NekDouble> > &outarray, const NekDouble time)
    {
        ASSERTL1(inarray.num_elements() == outarray.num_elements(),"Inarray and outarray of different sizes ");

        for(int i = 0; i < inarray.num_elements(); ++i)
        {
            Vmath::Vcopy(inarray[i].num_elements(),inarray[i],1,outarray[i],1);
        }
    }

    /* extrapolate field using equally time spaced field un,un-1,un-2, (at
       dt intervals) to time n+t at order Ord */
    void IncNavierStokes::SubStepExtrapoloteField(NekDouble toff, Array< OneD, Array<OneD, NekDouble> > &ExtVel)
    {
        int npts = m_fields[0]->GetTotPoints();
        int nvel = m_velocity.num_elements();
        int i,j;
        Array<OneD, NekDouble> l(4);

        int ord = m_intSteps;

        // calculate Lagrange interpolants
        Vmath::Fill(4,1.0,l,1);

        for(i = 0; i <= ord; ++i)
        {
            for(j = 0; j <= ord; ++j)
            {
                if(i != j)
                {
                    l[i] *= (j*m_timestep+toff);
                    l[i] /= (j*m_timestep-i*m_timestep);
                }
            }
        }

        for(i = 0; i < nvel; ++i)
        {
            Vmath::Smul(npts,l[0],m_previousVelFields[i],1,ExtVel[i],1);

            for(j = 1; j <= ord; ++j)
            {
                Blas::Daxpy(npts,l[j],m_previousVelFields[j*nvel+i],1,
                        ExtVel[i],1);
            }
        }
    }

    // Evaluation -N(V) for all fields except pressure using m_velocity
    void IncNavierStokes::EvaluateAdvectionTerms(const Array<OneD, const Array<OneD, NekDouble> > &inarray, 
            Array<OneD, Array<OneD, NekDouble> > &outarray, 
            Array<OneD, NekDouble> &wk)
    {
        int i;
        int nvariables = inarray.num_elements();
        int nqtot      = m_fields[0]->GetTotPoints();
        int VelDim     = m_velocity.num_elements();
        Array<OneD, Array<OneD, NekDouble> > velocity(VelDim);
        Array<OneD, NekDouble > Deriv;
        for(i = 0; i < VelDim; ++i)
        {
            velocity[i] = inarray[m_velocity[i]]; 
        }
        // Set up Derivative work space; 
        if(wk.num_elements())
        {
            ASSERTL0(wk.num_elements() >= nqtot*VelDim,"Workspace is not sufficient");            
            Deriv = wk;
        }
        else
        {
            Deriv = Array<OneD, NekDouble> (nqtot*VelDim);
        }

        m_advObject->DoAdvection(m_fields,m_nConvectiveFields, 
                m_velocity,inarray,outarray,m_time,Deriv);



    }

    void IncNavierStokes::WriteModalEnergy(void)
    {
        if(m_HomogeneousType != eNotHomogeneous)
        {
            if(m_HomogeneousType == eHomogeneous1D)
            {
                int colrank = m_comm->GetColumnComm()->GetRank();
                int nproc   = m_comm->GetColumnComm()->GetSize();
                int locsize = m_npointsZ/nproc/2;
                int ensize  = m_npointsZ/nproc/2;

                Array<OneD, NekDouble> energy    (locsize,0.0);
                Array<OneD, NekDouble> energy_tmp(locsize,0.0);
                Array<OneD, NekDouble> tmp;

                // Calculate modal energies.
                for(int i = 0; i < m_nConvectiveFields; ++i)
                {
                    energy_tmp = m_fields[i]->HomogeneousEnergy();
                    Vmath::Vadd(locsize,energy_tmp,1,energy,1,energy,1);
                }

                // Send to root process.
                if (colrank == 0)
                {
                    int j, m = 0;

                    for (j = 0; j < energy.num_elements(); ++j, ++m)
                    {
                        m_mdlFile << setw(10) << m_time 
                            << setw(5)  << m
                            << setw(18) << energy[j] << endl;
                    }

                    for (int i = 1; i < nproc; ++i)
                    {
                        m_comm->GetColumnComm()->Recv(i, energy);
                        for (j = 0; j < energy.num_elements(); ++j, ++m)
                        {
                            m_mdlFile << setw(10) << m_time 
                                << setw(5)  << m
                                << setw(18) << energy[j] << endl;
                        }
                    }
                }
                else
                {
                    m_comm->GetColumnComm()->Send(0, energy);
                }
            }
            else
            {
                ASSERTL0(false,"3D Homogeneous 2D energy dumping not implemented yet");
            }
        }
        else
        {
            NekDouble energy = 0.0;
            for(int i = 0; i < m_nConvectiveFields; ++i)
            {
                m_fields[i]->SetPhysState(true);
                NekDouble norm = L2Error(i, true);
                energy += norm*norm;
            }
            m_mdlFile << m_time << "   " << 0.5*energy << endl;
        }

    }

    //time dependent boundary conditions updating
    void IncNavierStokes::SetBoundaryConditions(NekDouble time)
    {
        int  nvariables = m_fields.num_elements();

        for (int i = 0; i < nvariables; ++i)
        {
            for(int n = 0; n < m_fields[i]->GetBndConditions().num_elements(); ++n)
            {	
                if(m_fields[i]->GetBndConditions()[n]->GetUserDefined() == SpatialDomains::eTimeDependent)
                {
                    m_fields[i]->EvaluateBoundaryConditions(time);
                }
            }
        }
    }

    // Decide if at a steady state if the discrerte L2 sum of the
    // coefficients is the same as the previous step to within the
    // tolerance m_steadyStateTol;
    bool IncNavierStokes::CalcSteadyState(void)
    {
        static NekDouble previousL2 = 0.0;
        bool returnval = false;

        NekDouble L2 = 0.0;

        // calculate L2 discrete summation 
        int ncoeffs = m_fields[0]->GetNcoeffs(); 

        for(int i = 0; i < m_fields.num_elements(); ++i)
        {
            L2 += Vmath::Dot(ncoeffs,m_fields[i]->GetCoeffs(),1,m_fields[i]->GetCoeffs(),1);
        }

        if(fabs(L2-previousL2) < ncoeffs*m_steadyStateTol)
        {
            returnval = true;
        }

        previousL2 = L2;

        return returnval;
    }

    NekDouble IncNavierStokes::GetSubstepTimeStep()
    { 
        int nvariables     = m_fields.num_elements();
        int nTotQuadPoints = GetTotPoints();
        int n_element      = m_fields[0]->GetExpSize(); 

        const Array<OneD, int> ExpOrder = GetNumExpModesPerExp();
        Array<OneD, int> ExpOrderList (n_element, ExpOrder);

        const NekDouble minLengthStdTri  = 1.414213;
        const NekDouble minLengthStdQuad = 2.0;
        const NekDouble cLambda          = 0.2; // Spencer book pag. 317

        Array<OneD, NekDouble> tstep      (n_element, 0.0);
        Array<OneD, NekDouble> stdVelocity(n_element, 0.0);
        Array<OneD, Array<OneD, NekDouble> > velfields(
                m_velocity.num_elements());

        for(int i = 0; i < m_velocity.num_elements(); ++i)
        {
            velfields[i] = m_fields[m_velocity[i]]->UpdatePhys();
        }        
        stdVelocity = GetStdVelocity(velfields);

        for(int el = 0; el < n_element; ++el)
        {
            int npoints = m_fields[0]->GetExp(el)->GetTotPoints();

            tstep[el] = m_cflSafetyFactor / 
                (stdVelocity[el] * cLambda * 
                 (ExpOrder[el]-1) * (ExpOrder[el]-1));
        }

        NekDouble TimeStep = Vmath::Vmin(n_element, tstep, 1);
        return TimeStep;
    }

    Array<OneD, NekDouble> IncNavierStokes::GetStdVelocity(
            const Array<OneD, Array<OneD,NekDouble> > inarray)
    {
        // Checking if the problem is 2D
        ASSERTL0(m_expdim >= 2, "Method not implemented for 1D");

        int nTotQuadPoints  = GetTotPoints();
        int n_element       = m_fields[0]->GetExpSize();       
        int nvel            = inarray.num_elements();
        int npts            = 0;

        NekDouble pntVelocity;

        // Getting the standard velocity vector on the 2D normal space
        Array<OneD, Array<OneD, NekDouble> > stdVelocity(nvel);
        Array<OneD, NekDouble> stdV(n_element, 0.0);

        for (int i = 0; i < nvel; ++i)
        {
            stdVelocity[i] = Array<OneD, NekDouble>(nTotQuadPoints);
        }

        if (nvel == 2)
        {
            for (int el = 0; el < n_element; ++el)
            { 

                int n_points = m_fields[0]->GetExp(el)->GetTotPoints();

                Array<OneD, const NekDouble> jac  = 
                    m_fields[0]->GetExp(el)->GetGeom2D()->GetJac();
                Array<TwoD, const NekDouble> gmat = 
                    m_fields[0]->GetExp(el)->GetGeom2D()->GetGmat();

                if (m_fields[0]->GetExp(el)->GetGeom2D()->GetGtype() 
                        == SpatialDomains::eDeformed)
                {
                    for (int i = 0; i < n_points; i++)
                    {
                        stdVelocity[0][i] = gmat[0][i]*inarray[0][i] 
                            + gmat[2][i]*inarray[1][i];

                        stdVelocity[1][i] = gmat[1][i]*inarray[0][i] 
                            + gmat[3][i]*inarray[1][i];
                    }
                }
                else
                {
                    for (int i = 0; i < n_points; i++)
                    {
                        stdVelocity[0][i] = gmat[0][0]*inarray[0][i] 
                            + gmat[2][0]*inarray[1][i];

                        stdVelocity[1][i] = gmat[1][0]*inarray[0][i] 
                            + gmat[3][0]*inarray[1][i];
                    }
                }


                for (int i = 0; i < n_points; i++)
                {
                    pntVelocity = sqrt(stdVelocity[0][i]*stdVelocity[0][i] 
                            + stdVelocity[1][i]*stdVelocity[1][i]);

                    if (pntVelocity>stdV[el])
                    {
                        stdV[el] = pntVelocity;
                    }
                }
            }
        }
        else
        {
            for (int el = 0; el < n_element; ++el)
            { 

                int n_points = m_fields[0]->GetExp(el)->GetTotPoints();

                Array<OneD, const NekDouble> jac =
                    m_fields[0]->GetExp(el)->GetGeom3D()->GetJac();
                Array<TwoD, const NekDouble> gmat =
                    m_fields[0]->GetExp(el)->GetGeom3D()->GetGmat();

                if (m_fields[0]->GetExp(el)->GetGeom3D()->GetGtype() 
                        == SpatialDomains::eDeformed)
                {
                    for (int i = 0; i < n_points; i++)
                    {
                        stdVelocity[0][i] = gmat[0][i]*inarray[0][i] 
                            + gmat[3][i]*inarray[1][i] 
                            + gmat[6][i]*inarray[2][i];

                        stdVelocity[1][i] = gmat[1][i]*inarray[0][i] 
                            + gmat[4][i]*inarray[1][i] 
                            + gmat[7][i]*inarray[2][i];

                        stdVelocity[2][i] = gmat[2][i]*inarray[0][i] 
                            + gmat[5][i]*inarray[1][i] 
                            + gmat[8][i]*inarray[2][i];
                    }
                }
                else
                {
                    Array<OneD, const NekDouble> jac =
                        m_fields[0]->GetExp(el)->GetGeom3D()->GetJac();
                    Array<TwoD, const NekDouble> gmat = 
                        m_fields[0]->GetExp(el)->GetGeom3D()->GetGmat();

                    for (int i = 0; i < n_points; i++)
                    {
                        stdVelocity[0][i] = gmat[0][0]*inarray[0][i] 
                            + gmat[3][0]*inarray[1][i] 
                            + gmat[6][0]*inarray[2][i];

                        stdVelocity[1][i] = gmat[1][0]*inarray[0][i] 
                            + gmat[4][0]*inarray[1][i] 
                            + gmat[7][0]*inarray[2][i];

                        stdVelocity[2][i] = gmat[2][0]*inarray[0][i] 
                            + gmat[5][0]*inarray[1][i] 
                            + gmat[8][0]*inarray[2][i];
                    }
                }

                for (int i = 0; i < n_points; i++)
                {
                    pntVelocity = sqrt(stdVelocity[0][i]*stdVelocity[0][i] 
                            + stdVelocity[1][i]*stdVelocity[1][i] 
                            + stdVelocity[2][i]*stdVelocity[2][i]);

                    if (pntVelocity > stdV[el])
                    {
                        stdV[el] = pntVelocity;
                    }
                }
            }
        }

        return stdV;
    }

} //end of namespace
