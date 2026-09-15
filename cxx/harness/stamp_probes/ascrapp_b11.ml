module P = Set.Make(String)
module M : sig end =
  struct
    module S = Set.Make(struct type t = int let compare = compare end)
  end
