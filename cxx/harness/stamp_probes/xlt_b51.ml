module F (X : sig module N : sig type t end end) = struct module M = struct
  include X end end
module A = struct module N = struct type t end end
module N = F (A)
