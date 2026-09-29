(* a shadowed included module type is not rebuilt *)
module X = struct module type S = sig type u end end
include X
module type S = sig end
module Y = struct type z end
