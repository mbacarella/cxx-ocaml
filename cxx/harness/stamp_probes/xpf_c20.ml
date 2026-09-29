module B = struct
  module rec Elt : sig type t end = struct type t = int end
end
