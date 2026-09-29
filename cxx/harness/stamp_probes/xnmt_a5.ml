module type S1 = sig val g : string -> string end
module type S2 = S1
module A : S2 = struct let g s = s end
let w = A.g ""
