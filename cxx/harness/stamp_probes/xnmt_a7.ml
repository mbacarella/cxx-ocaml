module A : sig val g : string -> string end = struct let g s = s end
let w = A.g ""
