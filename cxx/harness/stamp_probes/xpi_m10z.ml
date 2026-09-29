module A = struct type t = int end
module type S = sig type t end
let g (x : (module S with type t = int) option) = ()
