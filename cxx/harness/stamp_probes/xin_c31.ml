module A = struct module Make (M : sig end) = struct type t = string end end
module B = A.Make (struct end)
