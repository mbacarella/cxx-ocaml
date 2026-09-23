type t = int
module A : sig val g : t -> t end = struct let g x = x end
let w = A.g 0
