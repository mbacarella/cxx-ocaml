module A = struct type t = int end
module type S = sig type t end
let x = ((module A) : (module S with type t = int))
