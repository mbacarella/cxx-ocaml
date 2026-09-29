module type S = sig type t = int end
module type T = S with type t = string
