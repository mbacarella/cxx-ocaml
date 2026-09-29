module Outer = struct
  module type T = sig val g : string -> string end
end
module A : Outer.T = struct let g s = s end
let w = A.g ""
