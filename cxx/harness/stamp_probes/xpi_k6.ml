module A = struct type t = int end
module type S = sig type t end
let g = (module A : S with type t = int)
