module A = struct module Str = struct type t = string end
module Make (M : sig end) = struct include Str end end
module B = A.Make (struct end)
module C = A.Make (struct end)
