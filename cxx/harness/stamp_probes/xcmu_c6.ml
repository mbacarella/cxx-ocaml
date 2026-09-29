(* CONTROL: an arrow re-exported through `include` keeps the ORIGINAL's Cok *)
module M = struct let f (g : int -> int) = g 1 end
include M
