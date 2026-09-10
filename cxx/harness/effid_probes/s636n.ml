(* companion for s636: a unit whose signature is produced by `include M`,
   re-exporting a SUBMODULE.  Mtype.strengthen rewrites it to `module Set =
   M.Set` but keeps Mp_present -- the structure really builds the field. *)
module M = struct
  type t = string
  module Set = Set.Make (String)
end
include M
