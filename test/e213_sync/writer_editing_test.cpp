#include "../../src/targets/heltec_e213_writer/WriterEditing.h"
#include <cassert>
#include <string>

int main() {
  using namespace writerediting;
  {
    std::string doc;
    size_t cursor = 0;
    History h;
    assert(h.change(doc,cursor,0,0,"H",100,Kind::Insert));
    assert(h.change(doc,cursor,1,0,"e",180,Kind::Insert));
    assert(h.change(doc,cursor,2,0,"y",280,Kind::Insert));
    assert(doc == "Hey" && cursor == 3 && h.undoCount() == 1);
    assert(h.undo(doc,cursor) && doc.empty() && cursor == 0);
    assert(h.redo(doc,cursor) && doc == "Hey" && cursor == 3);
    assert(h.change(doc,cursor,3,0,"!",1100,Kind::Insert));
    assert(h.redoCount() == 0); // New edit invalidates redo.
    assert(h.undo(doc,cursor) && doc == "Hey");
    assert(h.undo(doc,cursor) && doc.empty());
    assert(h.redo(doc,cursor) && doc == "Hey");
  }
  {
    std::string doc = "hello";
    size_t cursor = doc.size();
    History h;
    assert(h.change(doc,cursor,4,1,"",100,Kind::Backspace));
    assert(h.change(doc,cursor,3,1,"",200,Kind::Backspace));
    assert(doc == "hel" && cursor == 3 && h.undoCount()==1);
    assert(h.undo(doc,cursor) && doc=="hello" && cursor==5);
    assert(h.redo(doc,cursor) && doc=="hel" && cursor==3);
    h.clear(); // Switch drafts: never undo across documents.
    assert(!h.undo(doc,cursor));
  }
  {
    std::string doc = "hello";
    size_t cursor = 0;
    History h;
    assert(h.change(doc,cursor,0,1,"",100,Kind::DeleteForward));
    assert(h.change(doc,cursor,0,1,"",200,Kind::DeleteForward));
    assert(doc=="llo" && h.undoCount()==1);
    assert(h.undo(doc,cursor) && doc=="hello" && cursor==0);
  }
  {
    std::string doc = "one two";
    size_t cursor = 4;
    History h;
    assert(h.change(doc,cursor,4,0,"very ",100,Kind::Other));
    assert(doc=="one very two");
    assert(h.undo(doc,cursor) && doc=="one two" && cursor==4);
    assert(h.redo(doc,cursor) && doc=="one very two");
    // An invalid history state never silently edits the wrong bytes.
    doc = "different";
    assert(!h.undo(doc,cursor));
  }
  {
    const std::string text="One sentence. Another one! Third?";
    assert(sentenceStart(text,text.find("Another")+4)==text.find("Another"));
    assert(sentenceEnd(text,text.find("Another")+4)==text.find('!')+1);
    assert(sentenceStart(text,5)==0);
    assert(sentenceEnd(text,5)==text.find('.')+1);
    assert(sentenceEnd(text,text.find("Third")+1)==text.size());
    assert(sentenceStart("No punctuation at all",15)==0);
    assert(sentenceEnd("No punctuation at all",4)==21);
  }
  {
    const std::string text="a\xC3\xA9" "b"; // aéb
    assert(previousCodepoint(text,3)==1);
    assert(previousCodepoint(text,4)==3);
    assert(nextCodepoint(text,1)==3);
    assert(nextCodepoint(text,4)==4);
  }
  {
    std::string doc="one";
    size_t cursor=3;
    History h;
    for(int i=0;i<120;++i) {
      assert(h.change(doc,cursor,cursor,0,"x",static_cast<uint32_t>(100+i*1000),Kind::Insert));
    }
    assert(h.undoCount()<=History::maxOperations);
    assert(doc.size()==123);
  }
}
