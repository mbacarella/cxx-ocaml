module Str = struct type t = string end
module Make (M : sig end) = struct type t = string end
module B = Make (struct end)
