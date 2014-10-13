#include "post_process/adminizer.hpp"

#include <mapnik/params.hpp>
#include <mapnik/datasource.hpp>
#include <mapnik/datasource_cache.hpp>
#include <mapnik/featureset.hpp>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/multi_point.hpp>
#include <boost/geometry/multi/geometries/multi_linestring.hpp>
#include <boost/geometry/index/rtree.hpp>

// NOTE: this is included only because it's where mapnik::coord2d is
// adapted to work with the boost::geometry stuff. we don't actually
// clip any polygons.
#include <mapnik/polygon_clipper.hpp>

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

using point_2d = bg::model::point<double, 2, bg::cs::cartesian>;
using box_2d = bg::model::box<point_2d>;
using linestring_2d = bg::model::linestring<point_2d>;
using multi_point_2d = bg::model::multi_point<point_2d>;
using multi_linestring_2d = bg::model::multi_linestring<linestring_2d>;
using polygon_2d = bg::model::polygon<point_2d>;

namespace {

typedef std::pair<box_2d, unsigned int> value;
struct entry {
  entry(polygon_2d &&p, mapnik::value &&v, unsigned int i)
    : polygon(p), value(v), index(i) {
  }
  polygon_2d polygon;
  mapnik::value value;
  unsigned int index;
};

struct param_updater {
  mapnik::feature_ptr &m_feature;
  const std::string &m_param_name;
  unsigned int m_index;
  bool m_finished;

  param_updater(mapnik::feature_ptr &feat, const std::string &param_name)
    : m_feature(feat), m_param_name(param_name)
    , m_index(std::numeric_limits<unsigned int>::max())
    , m_finished(false) {
  }

  void operator()(const entry &e) {
    if (e.index < m_index) {
      m_feature->put_new(m_param_name, e.value);
      m_finished = e.index == 0;
      m_index = e.index;
    }
  }
};

template <typename GeomType>
struct intersects_iterator {
  const GeomType &m_geom;
  const std::vector<entry> &m_entries;
  param_updater &m_updater;

  intersects_iterator(const GeomType &geom,
                      const std::vector<entry> &entries,
                      param_updater &updater)
    : m_geom(geom), m_entries(entries), m_updater(updater) {
  }

  intersects_iterator &operator++() { // prefix
    return *this;
  }

  intersects_iterator &operator*() {
    return *this;
  }

  intersects_iterator &operator=(const value &v) {
    const entry &e = m_entries[v.second];

    // do detailed intersection test, as the index only does bounding
    // box intersection tests.
    if (intersects(e.polygon)) {
      m_updater(e);
    }

    return *this;
  }

  bool intersects(const polygon_2d &) const;
};

template <>
bool intersects_iterator<multi_point_2d>::intersects(const polygon_2d &poly) const {
  // TODO: remove this hack when/if bg::intersects supports
  // intersection on multi type.
  for (auto point : m_geom) {
    if (bg::intersects(point, poly)) {
      return true;
    }
  }
  return false;
}

template <>
bool intersects_iterator<multi_linestring_2d>::intersects(const polygon_2d &poly) const {
  // TODO: remove this hack when/if bg::intersects supports
  // intersection on multi type.
  for (auto line : m_geom) {
    if (bg::intersects(line, poly)) {
      return true;
    }
  }
  return false;
}

template <>
bool intersects_iterator<polygon_2d>::intersects(const polygon_2d &poly) const {
  return bg::intersects(m_geom, poly);
}

template <typename RTreeType, typename GeomType>
void try_update(RTreeType &index,
                const GeomType &geom,
                const std::vector<entry> &entries,
                param_updater &updater) {

  intersects_iterator<GeomType> itr(geom, entries, updater);
  index.query(bgi::intersects(bg::return_envelope<box_2d>(geom)), itr);
}
  
} // anonymous namespace

namespace avecado {
namespace post_process {

using rtree = bgi::rtree<value, bgi::quadratic<16> >;

/**
 * Post-process that applies administrative region attribution
 * to features, based on geographic location of the geometry.
 */
class adminizer : public izer {
public:
  adminizer(pt::ptree const& config);
  virtual ~adminizer();

  virtual void process(std::vector<mapnik::feature_ptr> &layer) const;

private:
  mapnik::box2d<double> envelope(const std::vector<mapnik::feature_ptr> &layer) const;
  multi_point_2d make_boost_point(const mapnik::geometry_type &geom) const;
  multi_linestring_2d make_boost_linestring(const mapnik::geometry_type &geom) const;
  polygon_2d make_boost_polygon(const mapnik::geometry_type &geom) const;

  std::vector<entry> make_entries(const mapnik::box2d<double> &env) const;
  rtree make_index(const std::vector<entry> &entries) const;
  void adminize_feature(mapnik::feature_ptr &f,
                        const rtree &index,
                        const std::vector<entry> &entries) const;

  // the name of the parameter to take from the admin polygon and set
  // on the feature being adminized.
  std::string m_param_name;
  std::shared_ptr<mapnik::datasource> m_datasource;
};

adminizer::adminizer(pt::ptree const& config)
  : m_param_name(config.get<std::string>("param_name")) {
  mapnik::parameters params;

  boost::optional<pt::ptree const &> datasource_config =
    config.get_child_optional("datasource");

  if (datasource_config) {
    for (auto &kv : *datasource_config) {
      params[kv.first] = kv.second.data();
    }
  }

  m_datasource = mapnik::datasource_cache::instance().create(params);
}

adminizer::~adminizer() {
}

std::vector<entry> adminizer::make_entries(const mapnik::box2d<double> &env) const {
  // query the datasource
  // TODO: do we want to pass more things like scale denominator
  // and resolution type?
  mapnik::featureset_ptr fset = m_datasource->features(mapnik::query(env));

  std::vector<entry> entries;
  unsigned int index = 0;

  mapnik::feature_ptr f;
  while (f = fset->next()) {
    mapnik::value param = f->get(m_param_name);

    for (auto const &geom : f->paths()) {
      // ignore all non-polygon types
      if (geom.type() == mapnik::geometry_type::types::Polygon) {
        entries.emplace_back(make_boost_polygon(geom), std::move(param), index++);
      }
    }
  }

  return entries;
}

rtree adminizer::make_index(const std::vector<entry> &entries) const {
  // create envelope boxes for entries, as these are needed
  // up-front for the packing algorithm.
  std::vector<value> values;
  values.reserve(entries.size());
  const size_t num_entries = entries.size();
  for (size_t i = 0; i < num_entries; ++i) {
    values.emplace_back(bg::return_envelope<box_2d>(entries[i].polygon), i);
  }

  // construct index using packing algorithm, which leads to
  // better distribution for querying.
  return rtree(values.begin(), values.end());
}

void adminizer::adminize_feature(mapnik::feature_ptr &f,
                                 const rtree &index,
                                 const std::vector<entry> &entries) const {
  param_updater updater(f, m_param_name);

  for (auto const &geom : f->paths()) {
    if (geom.type() == mapnik::geometry_type::types::Point) {
      multi_point_2d multi_point = make_boost_point(geom);
      try_update(index, multi_point, entries, updater);

    } else if (geom.type() == mapnik::geometry_type::types::LineString) {
      multi_linestring_2d multi_line = make_boost_linestring(geom);
      try_update(index, multi_line, entries, updater);

    } else if (geom.type() == mapnik::geometry_type::types::Polygon) {
      polygon_2d poly = make_boost_polygon(geom);
      try_update(index, poly, entries, updater);
    }

    // quick exit the loop if there's nothing more to do.
    if (updater.m_finished) { break; }
  }
}

void adminizer::process(std::vector<mapnik::feature_ptr> &layer) const {
  // build extent of all features in layer
  mapnik::box2d<double> env = envelope(layer);

  // construct an index over the bounding boxes of the geometry,
  // first extracting the geometries from mapnik's representation
  // and transforming them too boost::geometry's representation.
  std::vector<entry> entries = make_entries(env);

  rtree index = make_index(entries);

  // loop over features, finding which items from the datasource
  // they intersect with.
  for (mapnik::feature_ptr f : layer) {
    adminize_feature(f, index, entries);
  }
}

mapnik::box2d<double> adminizer::envelope(const std::vector<mapnik::feature_ptr> &layer) const {
  mapnik::box2d<double> result;
  bool first = true;
  for (auto const &feature : layer) {
    if (first) {
      result = feature->envelope();

    } else {
      result.expand_to_include(feature->envelope());
    }
  }
  return result;
}

multi_point_2d adminizer::make_boost_point(const mapnik::geometry_type &geom) const {
/* Takes a mapnik geometry and makes a multi_point_2d from it. It has to be a
 * multipoint, since we don't know from geom.type() if it's a point or multipoint?
 */
  multi_point_2d points;
  double x = 0, y = 0;

  geom.rewind(0);

  unsigned int cmd = mapnik::SEG_END;

  while ((cmd = geom.vertex(&x, &y)) != mapnik::SEG_END) {
    points.push_back(bg::make<point_2d>(x, y));
  }
  return points;
}

multi_linestring_2d adminizer::make_boost_linestring(const mapnik::geometry_type &geom) const {
  multi_linestring_2d line;
  double x = 0, y = 0, prev_x = 0, prev_y = 0;

  geom.rewind(0);

  unsigned int cmd = mapnik::SEG_END;
  while ((cmd = geom.vertex(&x, &y)) != mapnik::SEG_END) {

    if (cmd == mapnik::SEG_MOVETO) {
      line.push_back(linestring_2d());
      line.back().push_back(bg::make<point_2d>(x, y));

    } else if (cmd == mapnik::SEG_LINETO) {
      if (std::abs(x - prev_x) < 1e-12 && std::abs(y - prev_y) < 1e-12) {
        continue;
      }

      line.back().push_back(bg::make<point_2d>(x, y));
    }

    prev_x = x;
    prev_y = y;
  }

  return line;
}

polygon_2d adminizer::make_boost_polygon(const mapnik::geometry_type &geom) const {
  polygon_2d poly;
  double x = 0, y = 0, prev_x = 0, prev_y = 0;
  unsigned int ring_count = 0;

  geom.rewind(0);

  unsigned int cmd = mapnik::SEG_END;
  while ((cmd = geom.vertex(&x, &y)) != mapnik::SEG_END) {

    if (cmd == mapnik::SEG_MOVETO) {
      if (ring_count == 0) {
        bg::append(poly, bg::make<point_2d>(x, y));

      } else {
        poly.inners().push_back(polygon_2d::inner_container_type::value_type());
        bg::append(poly.inners().back(), bg::make<point_2d>(x, y));
      }

      ++ring_count;

    } else if (cmd == mapnik::SEG_LINETO) {
      if (std::abs(x - prev_x) < 1e-12 && std::abs(y - prev_y) < 1e-12) {
        continue;
      }

      if (ring_count == 1) {
        bg::append(poly, bg::make<point_2d>(x, y));

      } else {
        bg::append(poly.inners().back(), bg::make<point_2d>(x, y));
      }
    }

    prev_x = x;
    prev_y = y;
  }

  return poly;
}

izer_ptr create_adminizer(pt::ptree const& config) {
  return std::make_shared<adminizer>(config);
}

} // namespace post_process
} // namespace avecado
