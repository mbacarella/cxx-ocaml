(* an unknown arrow re-exported through `include` stays unknown *)
module M = struct let f g = g 1 end
include M
