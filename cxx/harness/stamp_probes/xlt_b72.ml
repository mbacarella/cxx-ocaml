module F (X : sig type t type s end) = struct module M = struct module L = X
  end end
