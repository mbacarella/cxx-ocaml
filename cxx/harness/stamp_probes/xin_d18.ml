module A = struct module Str = struct type t = string end
module Make (M : sig end) = struct include Str end include Make (struct end)
end
type u = A.t
