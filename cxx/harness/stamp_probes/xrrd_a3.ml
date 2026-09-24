(* a local group re-exported by an include *)
module M = struct type t = unit -> u and u = Nil end
include M
