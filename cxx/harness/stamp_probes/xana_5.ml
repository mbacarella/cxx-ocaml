module F (X : sig type t end) = struct module M = struct type u = X.t end type w = { f : X.t } end
module N = F (struct type t end)
