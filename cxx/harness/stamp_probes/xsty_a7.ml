(* a shadowed included module is not rebuilt *)
module X = struct type t = int module N = struct type u end end
include X
module N = struct end
type t = float
