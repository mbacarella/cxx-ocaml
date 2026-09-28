(* G7: optional class parameters with defaults (ppxlib's
   Context_free.map_top_down): the label and the variable of `?(x = e)` are
   one string (parser.mly's label_var), and Typeclass's desugaring uses
   the literals "*opt*" / "*sth*", shared (-g debug events). *)
class c ?(first = 1) ?(second = "s") ?third () =
  object
    method sum = first + String.length second
    method third : int option = third
  end

let v = (new c ~first:2 ())#sum
