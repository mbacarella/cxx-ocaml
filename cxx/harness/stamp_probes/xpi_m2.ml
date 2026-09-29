module A = struct type t = int end
module type S = sig type t end
let g b = match b with 0 -> (module A : S with type t = int) | _ -> (module A)
