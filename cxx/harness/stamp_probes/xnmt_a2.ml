type t = int
module type S = sig val g : t -> t end
module A : S = struct let g x = x end
let w = A.g 0
