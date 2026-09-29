module A = struct module Str = struct type t = string end
module Make (M : sig end) = struct include Str end
module N = Make (struct end) end
type v = A.N.t
