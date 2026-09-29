module A = struct type t = int end
module type S = sig type t end
type r = { p : (module S with type t = int) }
