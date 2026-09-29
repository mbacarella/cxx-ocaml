module F (X : sig module N : sig type t end end) = struct include X end
module A = struct module N = struct type t end end
module R = F (A)
