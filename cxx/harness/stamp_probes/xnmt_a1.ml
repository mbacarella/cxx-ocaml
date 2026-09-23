module type S = sig val g : string -> string end
module A : S = struct let g s = s end
let w = A.g ""
