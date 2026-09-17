module A = struct module Str = struct type t = string end
module Make (M : sig end) = struct include Str let f s = s end
include Make (struct end) end
type u = A.t
