module F (X : sig type t end) = struct module M = struct include X end end
module N : sig module M : sig type t end end = F (struct type t end)
module O : sig end = F (struct type t end)
