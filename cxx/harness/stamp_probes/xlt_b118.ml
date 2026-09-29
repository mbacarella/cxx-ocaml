module F (X : sig module N : sig type t end end) = struct module M = struct
  module N = struct type t = X.N.t end end end
module A = struct module N = struct type t end end
module R = F (A)
