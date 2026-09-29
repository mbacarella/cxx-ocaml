let next () = 1
include (struct let a = next () let b = next () end
         : sig val a : int val b : int end)
include (struct let c = next () let d = next () end : sig val c : int end)
include (struct let c = next () let d = next () end : sig val d : int end)
let z = c + d
