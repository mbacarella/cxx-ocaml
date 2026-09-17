module A = struct module Str = struct type t = string end
module Make (M : sig end) = struct include Str type u = int end end
module B = A.Make (struct end)
