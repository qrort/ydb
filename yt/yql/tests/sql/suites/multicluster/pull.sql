/* postgres can not */
/* yt can not */
pragma yt.RuntimeCluster='banach';
pragma yt.RuntimeClusterSelection='auto';

select * from plato.PInput
union all
select * from banach.BInput
