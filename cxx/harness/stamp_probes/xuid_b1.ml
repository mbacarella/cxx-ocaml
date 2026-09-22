module type S = sig val x : int end
module Test (H : S) = struct let y = H.x end
let m1 = 0
module T2 (H : sig val x : int end) = struct let y = H.x end
let m2 = 0
module T3 (H : S) (K : S) = struct let y = H.x end
let m3 = 0
module T4 () = struct let y = 1 end
let m4 = 0
module T5 (_ : S) = struct let y = 1 end
let m5 = 0
