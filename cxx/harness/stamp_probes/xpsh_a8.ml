(* an external beside a spliced signature: both sharing laws in one unit *)
include Queue
external q : int -> int = "caml_probe_q"
let c : int t = create ()
