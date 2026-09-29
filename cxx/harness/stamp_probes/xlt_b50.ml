module F (X : sig module N : sig type t end end) = struct module M = struct
  include X end end
module N = F (struct module N = struct type t end end)
