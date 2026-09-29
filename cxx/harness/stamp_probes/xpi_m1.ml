module A = struct type t = int end
module type S = sig type t end
let g b = if b then (module A : S with type t = int) else (module A)
