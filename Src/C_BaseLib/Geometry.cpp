
//
// $Id: Geometry.cpp,v 1.54 2000-12-06 23:13:58 almgren Exp $
//

#include <Geometry.H>
#include <ParmParse.H>
#include <BoxArray.H>

//
// The definition of static data members.
//
RealBox Geometry::prob_domain;

bool Geometry::is_periodic[BL_SPACEDIM];

Geometry::FPBList Geometry::m_FPBCache;

ostream&
operator<< (ostream&        os,
            const Geometry& g)
{
    os << (CoordSys&) g << g.prob_domain << g.domain;
    return os;
}

istream&
operator>> (istream&  is,
            Geometry& g)
{
    is >> (CoordSys&) g >> g.prob_domain >> g.domain;
    return is;
}

ostream&
operator<< (ostream&               os,
            const Geometry::PIRec& pir)
{
    os << "mfi: "
       << pir.mfid
       << " from (Box "
       << pir.srcId
       << ") "
       << pir.srcBox
       << " to "
       << pir.dstBox;

    return os;
}

ostream&
operator<< (ostream&                 os,
	    const Geometry::PIRMMap& pirm)
{
    for (int i = 0; i < pirm.size(); i++)
        os << pirm[i] << '\n';
    return os;
}

int
Geometry::PIRMCacheSize ()
{
    return m_FPBCache.size();
}

void
Geometry::FlushPIRMCache ()
{
    m_FPBCache.clear();
}

Geometry::FPB&
Geometry::buildFPB (MultiFab&  mf,
                    const FPB& fpb) const
{
    BL_ASSERT(isAnyPeriodic());

    m_FPBCache.push_front(fpb);

    PIRMMap& pirm = m_FPBCache.front().m_pirm;

    Array<IntVect> pshifts(27);

    for (ConstMultiFabIterator mfi(mf); mfi.isValid(); ++mfi)
    {
        Box dest = mfi().box();

        BL_ASSERT(dest == ::grow(mfi.validbox(), mf.nGrow()));
//      BL_ASSERT(dest.ixType().cellCentered() || dest.ixType().nodeCentered());

        bool DoIt;
        Box  TheDomain;

        if (dest.ixType().cellCentered())
        {
            TheDomain = Domain();
            DoIt      = !Domain().contains(dest);
        }
        else if (dest.ixType().nodeCentered())
        {
            TheDomain = ::surroundingNodes(Domain());
            DoIt      = !::grow(TheDomain,-1).contains(dest);
        }
        else 
        {
            TheDomain = Domain();
            for (int n = 0; n < BL_SPACEDIM; n++)
              if (dest.ixType()[n] == IndexType::NODE)
                TheDomain.surroundingNodes(n);
            DoIt      = !TheDomain.contains(dest);
        }

        if (DoIt)
        {
            const BoxArray& grids = mf.boxArray();

            for (int j = 0; j < grids.length(); j++)
            {
                Box src = grids[j] & TheDomain;

                if (fpb.m_do_corners)
                {
                    for (int i = 0; i < BL_SPACEDIM; i++)
                    {
                        if (!isPeriodic(i))
                        {
                            if (src.smallEnd(i) == Domain().smallEnd(i))
                                src.growLo(i,mf.nGrow());
                            if (src.bigEnd(i) == Domain().bigEnd(i))
                                src.growHi(i,mf.nGrow());
                        }
                    }
                }

                periodicShift(dest, src, pshifts);

                for (int i = 0; i < pshifts.length(); i++)
                {
                    Box shftbox = src + pshifts[i];
                    Box src_box = dest & shftbox;
                    Box dst_box = src_box - pshifts[i];

                    pirm.push_back(PIRec(mfi.index(),j,dst_box,src_box));
                }
            }
        }
    }

    return m_FPBCache.front();
}

void
Geometry::FillPeriodicBoundary (MultiFab& mf,
                                int       scomp,
                                int       ncomp,
                                bool      corners) const
{
    if (!isAnyPeriodic()) return;

    static RunStats stats("fill_periodic_bndry");

    stats.start();

    MultiFabCopyDescriptor mfcd;

    FPB TheFPB(mf.boxArray(),Domain(),scomp,ncomp,mf.nGrow(),corners);

    const MultiFabId mfid = mfcd.RegisterMultiFab(&mf);
    FPB&             fpb  = getFPB(mf,TheFPB);
    PIRMMap&         pirm = fpb.m_pirm;
    //
    // Add boxes we need to collect.
    //
    for (int i = 0; i < pirm.size(); i++)
    {
        pirm[i].fbid = mfcd.AddBox(mfid,
                                   pirm[i].srcBox,
                                   0,
                                   pirm[i].srcId,
                                   scomp,
                                   scomp,
                                   ncomp,
                                   !corners);
    }

    mfcd.CollectData(&fpb.m_cache,&fpb.m_commdata);

    for (int i = 0; i < pirm.size(); i++)
    {
        BL_ASSERT(pirm[i].fbid.box() == pirm[i].srcBox);
        BL_ASSERT(pirm[i].srcBox.sameSize(pirm[i].dstBox));
        BL_ASSERT(mf.DistributionMap()[pirm[i].mfid] == ParallelDescriptor::MyProc());

        mfcd.FillFab(mfid, pirm[i].fbid, mf[pirm[i].mfid], pirm[i].dstBox);
    }

    stats.end();
}

Geometry::Geometry () {}

Geometry::Geometry (const Box& dom)
{
    define(dom);
}

Geometry::Geometry (const Geometry& g)
{
    domain = g.domain;
    ok     = g.ok;

    for (int k = 0; k < BL_SPACEDIM; k++)
    {
        dx[k] = g.dx[k];
    }
}

Geometry::~Geometry() {}

void
Geometry::define (const Box& dom)
{
    if (c_sys == undef)
        Setup();
    domain = dom;
    ok     = true;
    for (int k = 0; k < BL_SPACEDIM; k++)
    {
        dx[k] = prob_domain.length(k)/(Real(domain.length(k)));
    }
}

void
Geometry::Setup ()
{
    ParmParse pp("geometry");

    int coord;
    pp.get("coord_sys",coord);
    SetCoord( (CoordType) coord );

    Array<Real> prob_lo(BL_SPACEDIM);
    pp.getarr("prob_lo",prob_lo,0,BL_SPACEDIM);
    BL_ASSERT(prob_lo.length() == BL_SPACEDIM);
    Array<Real> prob_hi(BL_SPACEDIM);
    pp.getarr("prob_hi",prob_hi,0,BL_SPACEDIM);
    BL_ASSERT(prob_lo.length() == BL_SPACEDIM);
    prob_domain.setLo(prob_lo);
    prob_domain.setHi(prob_hi);
    //
    // Now get periodicity info.
    //
    D_EXPR(is_periodic[0]=0, is_periodic[1]=0, is_periodic[2]=0);
    if (pp.contains("period_0"))
        is_periodic[0] = 1;
#if BL_SPACEDIM>1
    if (pp.contains("period_1"))
        is_periodic[1] = 1;
#endif
#if BL_SPACEDIM>2
    if (pp.contains("period_2"))
        is_periodic[2] = 1;
#endif
}

void
Geometry::GetVolume (MultiFab&       vol,
                     const BoxArray& grds,
                     int             ngrow) const
{
    vol.define(grds,1,ngrow,Fab_noallocate);
    for (MultiFabIterator mfi(vol); mfi.isValid(); ++mfi)
    {
        Box gbx = ::grow(grds[mfi.index()],ngrow);
        vol.setFab(mfi.index(),CoordSys::GetVolume(gbx));
    }
}

#if (BL_SPACEDIM == 2)
void
Geometry::GetDLogA (MultiFab&       dloga,
                    const BoxArray& grds, 
                    int             dir,
                    int             ngrow) const
{
    dloga.define(grds,1,ngrow,Fab_noallocate);
    for (MultiFabIterator mfi(dloga); mfi.isValid(); ++mfi)
    {
        Box gbx = ::grow(grds[mfi.index()],ngrow);
        dloga.setFab(mfi.index(),CoordSys::GetDLogA(gbx,dir));
    }
}
#endif

void
Geometry::GetFaceArea (MultiFab&       area,
                       const BoxArray& grds,
                       int             dir,
                       int             ngrow) const
{
    BoxArray edge_boxes(grds);
    edge_boxes.surroundingNodes(dir);
    area.define(edge_boxes,1,ngrow,Fab_noallocate);
    for (MultiFabIterator mfi(area); mfi.isValid(); ++mfi)
    {
        Box gbx = ::grow(grds[mfi.index()],ngrow);
        area.setFab(mfi.index(),CoordSys::GetFaceArea(gbx,dir));
    }
}

void
Geometry::periodicShift (const Box&      target,
                         const Box&      src, 
                         Array<IntVect>& out) const
{
    Box locsrc(src);
    out.resize(0);

    int nist,njst,nkst;
    int niend,njend,nkend;
    nist = njst = nkst = 0;
    niend = njend = nkend = 0;
    D_TERM( nist , =njst , =nkst ) = -1;
    D_TERM( niend , =njend , =nkend ) = +1;

    int ri,rj,rk;
    for (ri = nist; ri <= niend; ri++)
    {
        if (ri != 0 && !is_periodic[0])
            continue;
        if (ri != 0 && is_periodic[0])
            locsrc.shift(0,ri*domain.length(0));

        for (rj = njst; rj <= njend; rj++)
        {
            if (rj != 0 && !is_periodic[1])
                continue;
            if (rj != 0 && is_periodic[1])
                locsrc.shift(1,rj*domain.length(1));

            for (rk = nkst; rk <= nkend; rk++)
            {
                if (rk!=0
#if (BL_SPACEDIM == 3)
                    && !is_periodic[2]
#endif
                    )
                {
                    continue;
                }
                if (rk!=0
#if (BL_SPACEDIM == 3)
                    && is_periodic[2]
#endif
                    )
                {
                    locsrc.shift(2,rk*domain.length(2));
                }

                if (ri == 0 && rj == 0 && rk == 0)
                    continue;
                //
                // If losrc intersects target, then add to "out".
                //
                if (target.intersects(locsrc))
                {
                    IntVect sh;
                    D_TERM(sh.setVal(0,ri*domain.length(0));,
                           sh.setVal(1,rj*domain.length(1));,
                           sh.setVal(2,rk*domain.length(2));)
                        out.resize(out.length()+1); 
                        out[out.length()-1] = sh;
                }
                if (rk != 0
#if (BL_SPACEDIM == 3)
                    && is_periodic[2]
#endif
                    )
                {
                    locsrc.shift(2,-rk*domain.length(2));
                }
            }
            if (rj != 0 && is_periodic[1])
                locsrc.shift(1,-rj*domain.length(1));
        }
        if (ri != 0 && is_periodic[0])
            locsrc.shift(0,-ri*domain.length(0));
    }
}
