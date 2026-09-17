module type S = sig type t end
module F (X : sig end) : sig include S end = struct type t = string end
