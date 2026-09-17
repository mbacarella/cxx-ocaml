module F (X : sig type t end) = struct module M = struct type t end end
module N : sig module M : sig type t end end = F (struct type t end)
