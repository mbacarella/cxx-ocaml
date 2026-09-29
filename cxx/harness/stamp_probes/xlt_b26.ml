module F (X : sig type t end) = struct include X end
module N = F (struct type t end)
