module type S = sig
  exception E of int (** doc *) [@deprecated]
end
