module A = struct module Str = struct type t = string end
module Make (M : sig end) = struct include Str end end
module M0 = struct end
module B = A.Make (M0)
