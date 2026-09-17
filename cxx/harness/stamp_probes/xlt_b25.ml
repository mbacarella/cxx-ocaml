module F (X : sig type t end) = struct include X end
module A = struct type t end
module N = F (A)
