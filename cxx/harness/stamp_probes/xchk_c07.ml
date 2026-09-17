module App (X : sig type t end) = struct end
module Y = App (struct type t module F (_ : sig end) = struct end end)
